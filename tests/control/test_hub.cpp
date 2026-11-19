// SPDX-License-Identifier: MIT
//
// The control hub with in-process links to real agents (no transport, no SIL Kit).
#include "test_harness.hpp"

#include "vehicle_sil/control/hub.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using namespace vehicle_sil;
using namespace vehicle_sil::control;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

/// A link that queues, and an in-process "transport" that delivers the queue to an agent and
/// reports back to the hub.
struct FakePeer
{
    struct Link final : AgentLink
    {
        std::mutex mutex;
        std::deque<Command> queue;
        bool up = true;
        bool closed = false; ///< the hub closed it (replaced by another connection)
        bool send(const Command& command) override
        {
            const std::lock_guard lock(mutex);
            if (!up)
            {
                return false;
            }
            queue.push_back(command);
            return true;
        }
        void close() override
        {
            const std::lock_guard lock(mutex);
            closed = true;
        }
    };

    std::string name;
    std::string incarnation;
    Hub& hub;
    double level = 0.0;
    bool lamp = false;
    double speed = 0.0;
    std::unique_ptr<Agent> agent;
    std::shared_ptr<Link> link = std::make_shared<Link>();
    ConnectionId connection = 0;

    /// With `reports`, "level" (Unknown at first) and "speed" (Inactive) report their availability.
    FakePeer(std::string n, std::string inc, Hub& h, std::size_t max_pending = 256, bool reports = false) :
      name(std::move(n)), incarnation(std::move(inc)), hub(h)
    {
        agent = std::make_unique<Agent>(AgentOptions{.max_pending = max_pending, .sample_period = SimDuration::zero()});
        const auto starts = [&](Availability state) { return reports ? std::optional(state) : std::nullopt; };
        agent->input("level", &level, {.unit = "%", .min = 0.0, .max = 100.0, .availability = starts(Availability::Unknown)});
        agent->input("lamp", &lamp);
        agent->output("speed", &speed, {.availability = starts(Availability::Inactive)});
    }

    PeerHello hello() const { return PeerHello{name, incarnation, agent->schema(), 4}; }

    /// Connects with a fresh link (a reconnect keeps the agent and its incarnation).
    void connect(std::optional<std::vector<std::uint64_t>> open_seqs = std::nullopt)
    {
        link = std::make_shared<Link>();
        auto h = hello();
        h.open_seqs = std::move(open_seqs);
        connection = hub.agent_connected(std::move(h), link);
    }

    /// Delivers what the link queued (the transport's job).
    void pump()
    {
        std::deque<Command> commands;
        {
            const std::lock_guard lock(link->mutex);
            commands.swap(link->queue);
        }
        for (auto& c : commands)
        {
            const auto seq = c.seq;
            hub.agent_submitted(connection, seq, agent->submit(std::move(c)));
        }
    }

    void step(SimTime now, bool report = true)
    {
        pump();
        agent->on_step_begin(now);
        speed += 1.0;
        agent->on_step_end(now);
        if (report)
        {
            this->report();
        }
    }

    /// What the transport sends of the agent's availability (after the schema is complete).
    void report_availability()
    {
        const auto states = agent->availability();
        hub.agent_availability(connection, agent->availability_version(), std::vector<Availability>(states.begin(), states.end()));
    }

    void report()
    {
        std::vector<CommandResult> results;
        agent->drain_results(results);
        for (const auto& r : results)
        {
            hub.agent_result(connection, r);
        }
        hub.agent_values(connection, agent->latest());
        std::vector<Event> events;
        agent->drain_events(events);
        hub.agent_events(connection, events);
    }
};

TargetResult target(const OperationResult& op, const std::string& peer)
{
    for (const auto& t : op.targets)
    {
        if (t.peer == peer)
        {
            return t;
        }
    }
    throw vehicle_sil::unit::Failure("no target " + peer);
}

TargetStatus status_of(Hub& hub, std::uint64_t seq, const std::string& peer)
{
    return target(*hub.operation(seq), peer).status;
}

OperationRequest req(std::vector<PeerWrite> writes, std::optional<SimTime> when = std::nullopt)
{
    return OperationRequest{std::move(writes), when, LatePolicy::Reject, "test"};
}

} // namespace

TEST_CASE(control_hub_routes_one_operation_to_several_peers)
{
    Hub hub(HubOptions{.first_seq = 100});
    FakePeer engine("Engine", "e1", hub), body("Body", "b1", hub);
    engine.connect();
    body.connect();
    CHECK_EQ(hub.peers().size(), std::size_t{2});

    const auto seq = hub.write(req({{"Engine", "level", 40.0}, {"Body", "lamp", true}, {"Engine", "lamp", true}}, at(25ms)));
    CHECK_EQ(seq, std::uint64_t{100});
    auto op = *hub.operation(seq);
    CHECK_EQ(op.targets.size(), std::size_t{2});
    CHECK(op.request.client == "test");
    CHECK(target(op, "Engine").status == TargetStatus::Sent);
    for (auto t = 0ms; t < 60ms; t += 10ms)
    {
        engine.step(at(t));
        if (t.count() % 20 == 0)
        {
            body.step(at(t));
        }
    }
    op = *hub.operation(seq);
    CHECK(op.done());
    CHECK(target(op, "Engine").applied_at == std::optional<SimTime>(at(30ms)));
    CHECK(target(op, "Body").applied_at == std::optional<SimTime>(at(40ms)));
    CHECK_EQ(engine.level, 40.0);
    CHECK(engine.lamp);
    CHECK(body.lamp);
    CHECK_EQ(hub.write(req({{"Engine", "lamp", false}})), std::uint64_t{101});
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { hub.write(req({})); }));
}

TEST_CASE(control_hub_refuses_what_it_cannot_send)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    const auto check = [&](std::vector<PeerWrite> writes, const char* peer, TargetStatus expected) {
        const auto op = *hub.operation(hub.write(req(std::move(writes))));
        CHECK(op.done());
        CHECK(target(op, peer).status == expected);
        CHECK(!target(op, peer).reason.empty());
    };
    check({{"Nobody", "level", 1.0}}, "Nobody", TargetStatus::Invalid);
    check({{"Engine", "nope", 1.0}}, "Engine", TargetStatus::Invalid);
    check({{"Engine", "speed", 1.0}}, "Engine", TargetStatus::Invalid); // an output
    const auto seq = hub.write(req({{"Engine", "level", 500.0}}));     // the agent's own range check
    engine.pump();
    CHECK(status_of(hub, seq, "Engine") == TargetStatus::Invalid);
    engine.link->up = false;
    check({{"Engine", "level", 1.0}}, "Engine", TargetStatus::Invalid);
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { hub.agent_connected(PeerHello{}, engine.link); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { hub.agent_connected(PeerHello{"X", "x", {}, 4}, nullptr); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { hub.agent_connected(PeerHello{"X", "x", {}, 0}, engine.link); }));
}

TEST_CASE(control_hub_bounds_the_open_commands_per_peer)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub); // max_pending 4 in its hello
    engine.connect();
    std::vector<std::uint64_t> seqs;
    for (int i = 0; i < 6; ++i)
    {
        seqs.push_back(hub.write(req({{"Engine", "level", 1.0}}, at(1s))));
    }
    CHECK(status_of(hub, seqs[3], "Engine") == TargetStatus::Sent);
    CHECK(status_of(hub, seqs[4], "Engine") == TargetStatus::Busy);
    CHECK(status_of(hub, seqs[5], "Engine") == TargetStatus::Busy);
}

TEST_CASE(control_hub_reconnect_of_the_same_process)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    const auto first = engine.connection;
    const auto queued = hub.write(req({{"Engine", "level", 1.0}}, at(20ms)));
    engine.pump();
    const auto unsent = hub.write(req({{"Engine", "level", 2.0}}, at(30ms))); // handed to the link, not queued
    hub.agent_disconnected(first);
    CHECK(status_of(hub, queued, "Engine") == TargetStatus::Disconnected);
    CHECK(status_of(hub, unsent, "Engine") == TargetStatus::Unknown);
    CHECK(!hub.peers().at(0).connected);
    CHECK(status_of(hub, hub.write(req({{"Engine", "level", 3.0}})), "Engine") == TargetStatus::Invalid);

    // The agent applies the queued command while disconnected, reconnects (same incarnation) and
    // reports the result: it is taken.
    engine.agent->on_step_begin(at(20ms));
    engine.connect();
    engine.report();
    CHECK(status_of(hub, queued, "Engine") == TargetStatus::Applied);

    // The process reconnects again while its previous connection is still open, and the notice
    // that the previous connection closed arrives afterwards: the peer stays connected.
    const auto second = engine.connection;
    const auto second_link = engine.link;
    engine.connect();
    CHECK(second_link->closed); // the replaced connection is ended, not left open
    CHECK(!engine.link->closed);
    hub.agent_disconnected(second);
    CHECK(hub.peers().at(0).connected);
    hub.agent_disconnected(first); // long gone: no effect either
    CHECK(hub.peers().at(0).connected);
    const auto after = hub.write(req({{"Engine", "lamp", true}}));
    CHECK(status_of(hub, after, "Engine") == TargetStatus::Sent);
}

TEST_CASE(control_hub_restart_of_the_peer)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    engine.step(at(0ms)); // a sample of e1
    auto sub = hub.subscribe();
    static_cast<void>(sub->poll(0, 0ms));
    const auto open = hub.write(req({{"Engine", "level", 3.0}}, at(50ms)));
    engine.pump();
    engine.agent->on_step_end(at(1ms));
    hub.agent_values(engine.connection, engine.agent->latest()); // an unread sample of e1

    FakePeer restarted("Engine", "e2", hub);
    CHECK(!engine.link->closed);
    restarted.connect();
    // Also another incarnation (perhaps a second process with the same name that still runs)
    // ends the old connection, so that the old process learns it was replaced.
    CHECK(engine.link->closed);
    CHECK(!restarted.link->closed);
    CHECK(status_of(hub, open, "Engine") == TargetStatus::Unknown);
    CHECK(hub.peers().at(0).incarnation == "e2");
    CHECK(hub.latest("Engine") == nullptr); // e1's sample does not follow e2's schema
    // A result claiming to be from the old process does not settle a command for the new one.
    const auto to_new = hub.write(req({{"Engine", "level", 4.0}}));
    hub.agent_result(engine.connection, CommandResult{to_new, CommandStatus::Applied, at(60ms), false, {}});
    CHECK(status_of(hub, to_new, "Engine") == TargetStatus::Sent);
    // The subscriber sees the new incarnation and no value of the old one.
    for (const auto& u : sub->poll(0, 0ms))
    {
        if (const auto* v = std::get_if<ValuesUpdate>(&u))
        {
            CHECK(v->incarnation == "e2");
        }
    }
    // Values of the old connection are ignored; a sample that does not match the schema too.
    hub.agent_values(engine.connection, engine.agent->latest());
    CHECK(hub.latest("Engine") == nullptr);
    Snapshot wrong;
    wrong.sequence = 9;
    wrong.values = {Value{1.0}};
    hub.agent_values(restarted.connection, wrong);
    CHECK(hub.latest("Engine") == nullptr);
    restarted.step(at(0ms));
    CHECK(hub.latest("Engine") != nullptr);
    // An older sample does not replace a newer one.
    auto older = *hub.latest("Engine");
    older.sequence = 0;
    hub.agent_values(restarted.connection, older);
    CHECK(hub.latest("Engine")->sequence == 1);
}

TEST_CASE(control_hub_forgets_disconnected_peers)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    const auto seq = hub.write(req({{"Engine", "level", 1.0}}, at(1s)));
    engine.pump();
    CHECK(!hub.forget_peer("Engine")); // still connected
    hub.agent_disconnected(engine.connection);
    CHECK(status_of(hub, seq, "Engine") == TargetStatus::Disconnected);
    CHECK(hub.forget_peer("Engine"));
    CHECK(status_of(hub, seq, "Engine") == TargetStatus::Unknown);
    CHECK(hub.peers().empty());
}

TEST_CASE(control_hub_subscriptions_keep_the_latest_and_bound_the_rest)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub), body("Body", "b1", hub);
    engine.connect();
    engine.step(at(0ms));
    // A new subscriber gets the peers and the latest values first, beyond its capacity (here no
    // newer sample comes: the value is the one the hub kept).
    {
        auto late = hub.subscribe(SubscriptionOptions{.capacity = 1});
        const auto first = late->poll(0, 0ms);
        CHECK_EQ(first.size(), std::size_t{2});
        CHECK(std::holds_alternative<PeerUpdate>(first.at(0)));
        CHECK(std::get<ValuesUpdate>(first.at(1)).snapshot->time == at(0ms));
    }
    auto all = hub.subscribe(SubscriptionOptions{.capacity = 1});
    auto only_body = hub.subscribe(SubscriptionOptions{.peers = {"Body"}, .values = false});
    body.connect();
    for (auto t = 1ms; t < 50ms; t += 1ms)
    {
        engine.step(at(t));
    }
    auto updates = all->poll(0, 0ms);
    std::size_t values = 0;
    std::size_t peers = 0;
    for (const auto& u : updates)
    {
        if (const auto* v = std::get_if<ValuesUpdate>(&u))
        {
            ++values;
            CHECK(v->peer == "Engine");
            CHECK(v->snapshot->time == at(49ms));
        }
        peers += std::holds_alternative<PeerUpdate>(u) ? 1 : 0;
    }
    CHECK_EQ(values, std::size_t{1});
    CHECK_EQ(peers, std::size_t{2});
    CHECK_EQ(all->dropped(), std::uint64_t{0});
    // Events beyond the capacity drop the oldest and count them.
    std::vector<Event> burst(10, Event{at(1ms), EventLevel::Info, "e"});
    hub.agent_events(engine.connection, burst);
    updates = all->poll(0, 0ms);
    CHECK_EQ(updates.size(), std::size_t{1});
    CHECK_EQ(all->dropped(), std::uint64_t{9});
    // An operation's progress is kept as its latest state: the final one is never lost.
    const auto seq = hub.write(req({{"Body", "lamp", true}}));
    body.step(at(0ms));
    hub.agent_events(engine.connection, burst); // a burst that does not touch the operation
    updates = only_body->poll(0, 0ms);
    std::optional<OperationResult> seen;
    for (const auto& u : updates)
    {
        if (const auto* o = std::get_if<OperationUpdate>(&u))
        {
            seen = o->operation;
        }
        if (const auto* p = std::get_if<PeerUpdate>(&u))
        {
            CHECK(p->peer.participant == "Body");
        }
    }
    CHECK(seen.has_value());
    CHECK_EQ(seen->seq, seq);
    CHECK(seen->targets.at(0).status == TargetStatus::Applied);
    // Peer status changes reach subscribers.
    hub.agent_status(body.connection, PeerStatus{true, "boom"});
    updates = only_body->poll(0, 0ms);
    CHECK_EQ(updates.size(), std::size_t{1});
    CHECK(std::get<PeerUpdate>(updates[0]).peer.status.failed);
    // close() wakes a waiting poll.
    std::thread closer([&] {
        std::this_thread::sleep_for(20ms);
        only_body->close();
    });
    const auto begin = std::chrono::steady_clock::now();
    CHECK(only_body->poll(0, 5s).empty());
    CHECK(std::chrono::steady_clock::now() - begin < 3s);
    closer.join();
}

TEST_CASE(control_hub_wait_returns_the_operation)
{
    Hub hub(HubOptions{.max_operations = 1});
    FakePeer engine("Engine", "e1", hub), busy("Busy", "x1", hub, 1);
    engine.connect();
    busy.connect();
    const auto seq = hub.write(req({{"Engine", "level", 10.0}}, at(30ms)));
    CHECK(!hub.wait_queued(seq, 10ms).has_value());
    std::thread transport([&] {
        std::this_thread::sleep_for(20ms);
        engine.pump();
    });
    const auto queued = hub.wait_queued(seq, scaled_timeout(5s));
    transport.join();
    CHECK(queued.has_value());
    CHECK(queued->targets.at(0).status == TargetStatus::Queued);
    engine.step(at(30ms));
    // The result comes with the wait, even if the operation is forgotten right after.
    const auto done = hub.wait_done(seq, scaled_timeout(5s));
    CHECK(done.has_value());
    CHECK(done->targets.at(0).status == TargetStatus::Applied);
    hub.write(req({{"Nobody", "x", 1.0}}));
    hub.write(req({{"Nobody", "x", 1.0}}));
    CHECK(!hub.operation(seq).has_value());
    CHECK(!hub.wait_done(seq, 10ms).has_value()); // unknown seq: at once
    // A Busy target is not "queued": the caller sees it in the returned operation.
    const auto first = hub.write(req({{"Busy", "level", 1.0}}, at(1s)));
    busy.pump();
    const auto mixed = hub.write(req({{"Engine", "level", 1.0}, {"Busy", "level", 2.0}}, at(1s)));
    engine.pump();
    busy.pump(); // the agent refuses it: its one slot is taken
    const auto w = hub.wait_queued(mixed, scaled_timeout(1s));
    CHECK(w.has_value());
    CHECK(target(*w, "Engine").status == TargetStatus::Queued);
    CHECK(target(*w, "Busy").status == TargetStatus::Busy);
    static_cast<void>(first);
}

namespace {

/// A link that delivers to the agent and reports to the hub from inside send(): allowed, because
/// the hub calls send() without its lock.
struct DirectLink final : AgentLink
{
    Hub* hub = nullptr;
    Agent* agent = nullptr;
    ConnectionId connection = 0;
    bool send(const Command& command) override
    {
        hub->agent_submitted(connection, command.seq, agent->submit(command));
        return true;
    }
};

} // namespace

TEST_CASE(control_hub_a_link_may_call_back_into_the_hub)
{
    Hub hub;
    double level = 0.0;
    Agent agent;
    agent.input("level", &level);
    auto link = std::make_shared<DirectLink>();
    link->hub = &hub;
    link->agent = &agent;
    link->connection = hub.agent_connected(PeerHello{"Direct", "d1", agent.schema(), 256}, link);
    const auto seq = hub.write(req({{"Direct", "level", 5.0}}));
    CHECK(status_of(hub, seq, "Direct") == TargetStatus::Queued);
}

TEST_CASE(control_hub_clients_transports_and_subscribers_on_many_threads)
{
    // Two clients write, a transport thread delivers and steps two peers, a subscriber reads:
    // every operation finishes Applied (run under ThreadSanitizer).
    Hub hub;
    FakePeer engine("Engine", "e1", hub), body("Body", "b1", hub);
    engine.connect();
    body.connect();
    auto sub = hub.subscribe(SubscriptionOptions{.capacity = 64});
    constexpr int kPerClient = 300;
    std::atomic<bool> stop{false};
    std::atomic<int> not_applied{0};
    std::thread reader([&] {
        while (!stop.load())
        {
            static_cast<void>(sub->poll(32, 1ms));
        }
    });
    std::thread transport([&] {
        for (auto t = 0ms; !stop.load(); t += 1ms)
        {
            engine.step(at(t));
            body.step(at(t));
        }
    });
    std::thread clients[2];
    for (auto& client : clients)
    {
        client = std::thread([&] {
            for (int i = 0; i < kPerClient; ++i)
            {
                const auto seq = hub.write(req({{"Engine", "level", static_cast<double>(i % 100)}, {"Body", "lamp", i % 2 == 0}}));
                const auto done = hub.wait_done(seq, scaled_timeout(10s));
                if (!done || done->targets.at(0).status != TargetStatus::Applied || done->targets.at(1).status != TargetStatus::Applied)
                {
                    ++not_applied;
                }
            }
        });
    }
    for (auto& client : clients)
    {
        client.join();
    }
    stop = true;
    transport.join();
    reader.join();
    CHECK_EQ(not_applied.load(), 0);
}

TEST_CASE(control_hub_reconnects_that_lost_results_do_not_pile_up)
{
    // The transport loses results on a breaking link, again and again. The agent's process lives
    // on; at each reconnect the hello lists what the agent still holds, so the rest is settled
    // (Unknown) and the peer never runs out of room.
    Hub hub;
    FakePeer engine("Engine", "e1", hub); // max_pending 4 in its hello
    engine.connect();
    for (int round = 0; round < 10; ++round)
    {
        const auto lost = hub.write(req({{"Engine", "level", 1.0}}));
        const auto kept = hub.write(req({{"Engine", "level", 2.0}}, at(1h)));
        engine.pump();
        engine.step(at(std::chrono::milliseconds(round * 10 + 1)), false); // applies `lost`; its result is not reported
        std::vector<CommandResult> dropped;
        engine.agent->drain_results(dropped); // ... and lost with the link
        hub.agent_disconnected(engine.connection);
        CHECK(status_of(hub, lost, "Engine") == TargetStatus::Disconnected);
        engine.connect(std::vector<std::uint64_t>{kept}); // the agent still holds `kept`
        CHECK(status_of(hub, lost, "Engine") == TargetStatus::Unknown);
        CHECK(status_of(hub, kept, "Engine") == TargetStatus::Queued); // the agent holds it
        engine.agent->finish(); // a fresh agent for the next round would be a new process; here we just cancel `kept`
        engine.report();
        CHECK(status_of(hub, kept, "Engine") == TargetStatus::Cancelled);
        engine.agent = std::make_unique<Agent>(AgentOptions{.sample_period = SimDuration::zero()});
        engine.agent->input("level", &engine.level);
        engine.agent->input("lamp", &engine.lamp);
        engine.agent->output("speed", &engine.speed);
    }
    CHECK(status_of(hub, hub.write(req({{"Engine", "lamp", true}})), "Engine") == TargetStatus::Sent);
}

TEST_CASE(control_hub_commands_on_a_replaced_connection_are_settled_by_its_disconnect)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    const auto old_connection = engine.connection;
    const auto in_flight = hub.write(req({{"Engine", "level", 1.0}})); // taken by the old link, never delivered
    engine.connect(); // the same process reconnects before the old link reports its end
    CHECK(status_of(hub, in_flight, "Engine") == TargetStatus::Sent);
    hub.agent_disconnected(old_connection);
    CHECK(status_of(hub, in_flight, "Engine") == TargetStatus::Unknown);
    CHECK(hub.peers().at(0).connected);
}

namespace {

/// A link that disconnects itself from inside send() and whose destructor calls the hub from
/// another thread and waits for it: it must not run under the hub's lock.
struct SelfDisconnectingLink final : AgentLink
{
    Hub* hub = nullptr;
    ConnectionId connection = 0;
    std::atomic<bool>* destroyed_ok = nullptr;
    bool send(const Command&) override
    {
        hub->agent_disconnected(connection);
        return true;
    }
    ~SelfDisconnectingLink() override
    {
        std::atomic<bool> done{false};
        std::thread other([&] {
            static_cast<void>(hub->peers());
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(2s);
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        *destroyed_ok = done.load();
        if (done.load())
        {
            other.join();
        }
        else
        {
            other.detach(); // the hub's lock is held: report it rather than hang
        }
    }
};

/// A slow link that records the order of the commands and whether two send() calls overlapped.
struct OrderLink final : AgentLink
{
    std::mutex mutex;
    std::vector<std::uint64_t> seqs;
    std::atomic<int> inside{0};
    std::atomic<bool> overlapped{false};
    bool send(const Command& command) override
    {
        if (inside.fetch_add(1) != 0)
        {
            overlapped = true;
        }
        std::this_thread::sleep_for(200us);
        {
            const std::lock_guard lock(mutex);
            seqs.push_back(command.seq);
        }
        inside.fetch_sub(1);
        return true;
    }
};

} // namespace

TEST_CASE(control_hub_releases_a_link_outside_its_lock)
{
    Hub hub;
    std::atomic<bool> destroyed_ok{false};
    {
        auto link = std::make_shared<SelfDisconnectingLink>();
        link->hub = &hub;
        link->destroyed_ok = &destroyed_ok;
        Agent agent;
        double level = 0.0;
        agent.input("level", &level);
        link->connection = hub.agent_connected(PeerHello{"Self", "s1", agent.schema(), 8}, link);
        link.reset(); // the hub holds the only reference
        const auto seq = hub.write(req({{"Self", "level", 1.0}}));
        CHECK(status_of(hub, seq, "Self") == TargetStatus::Unknown); // lost while being sent
    }
    CHECK(destroyed_ok.load());
}

TEST_CASE(control_hub_sends_to_a_peer_in_order_and_one_at_a_time)
{
    Hub hub;
    auto link = std::make_shared<OrderLink>();
    Agent agent;
    double level = 0.0;
    agent.input("level", &level);
    hub.agent_connected(PeerHello{"Slow", "s1", agent.schema(), 100000}, link);
    std::thread clients[3];
    for (auto& client : clients)
    {
        client = std::thread([&] {
            for (int i = 0; i < 100; ++i)
            {
                hub.write(req({{"Slow", "level", 1.0}}));
            }
        });
    }
    for (auto& client : clients)
    {
        client.join();
    }
    CHECK(!link->overlapped.load());
    CHECK_EQ(link->seqs.size(), std::size_t{300});
    CHECK(std::is_sorted(link->seqs.begin(), link->seqs.end()));
}

TEST_CASE(control_hub_tells_subscribers_about_a_forgotten_peer)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    engine.step(at(0ms));
    auto sub = hub.subscribe();
    hub.agent_disconnected(engine.connection);
    CHECK(hub.forget_peer("Engine"));
    const auto updates = sub->poll(0, 0ms);
    CHECK_EQ(updates.size(), std::size_t{1}); // the removal replaced the unread peer and its values
    const auto& p = std::get<PeerUpdate>(updates.at(0));
    CHECK(p.removed);
    CHECK(p.peer.participant == "Engine");
}

TEST_CASE(control_hub_a_full_subscription_drops_the_operation_that_changed_longest_ago)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub), body("Body", "b1", hub);
    engine.connect();
    body.connect();
    auto sub = hub.subscribe(SubscriptionOptions{.values = false, .events = false, .capacity = 2});
    static_cast<void>(sub->poll(0, 0ms)); // the peers
    const auto a = hub.write(req({{"Engine", "level", 1.0}}, at(1h)));
    const auto b = hub.write(req({{"Body", "level", 1.0}}, at(1h)));
    engine.pump(); // a changes again (Queued): b is now the one that changed longest ago
    const auto c = hub.write(req({{"Nobody", "x", 1.0}}));
    std::vector<std::uint64_t> seen;
    for (const auto& u : sub->poll(0, 0ms))
    {
        seen.push_back(std::get<OperationUpdate>(u).operation.seq);
    }
    CHECK((seen == std::vector<std::uint64_t>{a, c}));
    CHECK_EQ(sub->dropped(), std::uint64_t{1});
    static_cast<void>(b);
}

TEST_CASE(control_hub_a_subscription_leaves_out_the_operations_of_the_clients_it_names)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    auto every = hub.subscribe(SubscriptionOptions{.values = false, .events = false});
    // Room for one: what it leaves out neither takes room nor counts as dropped.
    auto without = hub.subscribe(
        SubscriptionOptions{.values = false, .events = false, .operation_clients_excluded = {"scenario", "other"}, .capacity = 1});
    static_cast<void>(every->poll(0, 0ms)); // the peers
    static_cast<void>(without->poll(0, 0ms));
    const auto from = [](std::string client) {
        auto request = req({{"Engine", "level", 1.0}}, at(1h));
        request.client = std::move(client);
        return request;
    };
    const auto a = hub.write(from("scenario"));
    const auto b = hub.write(from("test"));
    const auto c = hub.write(from("other"));
    engine.pump(); // each changes again (Queued)
    const auto seqs = [](const std::shared_ptr<Subscription>& sub) {
        std::vector<std::uint64_t> seen;
        for (const auto& u : sub->poll(0, 0ms))
        {
            seen.push_back(std::get<OperationUpdate>(u).operation.seq);
        }
        return seen;
    };
    CHECK((seqs(every) == std::vector<std::uint64_t>{a, b, c}));
    CHECK((seqs(without) == std::vector<std::uint64_t>{b}));
    CHECK_EQ(without->dropped(), std::uint64_t{0});
    CHECK_EQ(every->dropped(), std::uint64_t{0});
}

namespace {

/// A link whose send() lets the peer restart (a new incarnation connects) while it sends.
struct RestartingLink final : AgentLink
{
    Hub* hub = nullptr;
    std::shared_ptr<AgentLink> next;
    std::vector<VariableInfo> schema;
    bool restarted = false;
    bool send(const Command&) override
    {
        if (!restarted)
        {
            restarted = true;
            hub->agent_connected(PeerHello{"Peer", "p2", schema, 8}, next);
        }
        return true;
    }
};

struct CountingLink final : AgentLink
{
    std::atomic<int> sent{0};
    bool send(const Command&) override
    {
        ++sent;
        return true;
    }
};

} // namespace

TEST_CASE(control_hub_a_restart_during_send_does_not_stall_the_peer)
{
    Hub hub;
    Agent agent;
    double level = 0.0;
    agent.input("level", &level);
    auto next = std::make_shared<CountingLink>();
    auto first = std::make_shared<RestartingLink>();
    first->hub = &hub;
    first->next = next;
    first->schema = agent.schema();
    hub.agent_connected(PeerHello{"Peer", "p1", agent.schema(), 8}, first);
    const auto s1 = hub.write(req({{"Peer", "level", 1.0}})); // the peer restarts while this is sent
    CHECK(status_of(hub, s1, "Peer") == TargetStatus::Unknown);
    for (int i = 0; i < 4; ++i)
    {
        const auto s = hub.write(req({{"Peer", "level", 2.0}}));
        CHECK(status_of(hub, s, "Peer") == TargetStatus::Sent);
    }
    CHECK_EQ(next->sent.load(), 4); // the new incarnation's link got them
}

TEST_CASE(control_hub_open_seqs_survive_the_end_of_the_old_connection)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    const auto old_connection = engine.connection;
    const auto s1 = hub.write(req({{"Engine", "level", 1.0}}, at(10ms)));
    // The agent queues it, but its SubmitResult is lost with the old connection.
    {
        std::deque<Command> commands;
        const std::lock_guard lock(engine.link->mutex);
        commands.swap(engine.link->queue);
        CHECK(engine.agent->submit(commands.at(0)).status == SubmitStatus::Queued);
    }
    engine.connect(std::vector<std::uint64_t>{s1}); // the agent says it holds s1
    CHECK(status_of(hub, s1, "Engine") == TargetStatus::Queued);
    hub.agent_disconnected(old_connection); // the old connection's end arrives late
    CHECK(status_of(hub, s1, "Engine") == TargetStatus::Queued);
    engine.step(at(10ms)); // applies it; the result comes on the new connection
    CHECK(status_of(hub, s1, "Engine") == TargetStatus::Applied);
}

TEST_CASE(control_hub_the_end_of_an_agent_settles_what_is_open_for_it)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    FakePeer brake("Brake", "b1", hub);
    engine.connect();
    brake.connect();
    const auto queued = hub.write(req({{"Engine", "level", 1.0}, {"Brake", "level", 1.0}}, at(20ms)));
    engine.pump();
    brake.pump();
    const auto unsent = hub.write(req({{"Engine", "level", 2.0}}, at(30ms))); // handed to the link, not queued
    const auto done = hub.write(req({{"Engine", "lamp", true}}));
    engine.step(at(10ms));
    CHECK(status_of(hub, done, "Engine") == TargetStatus::Applied);

    // The transport knows that this incarnation will not come back: nothing stays open for it.
    hub.agent_ended(engine.connection, "the participant left");
    CHECK(status_of(hub, queued, "Engine") == TargetStatus::Unknown);
    CHECK(target(*hub.operation(queued), "Engine").reason == "the participant left");
    CHECK(status_of(hub, unsent, "Engine") == TargetStatus::Unknown);
    CHECK(status_of(hub, done, "Engine") == TargetStatus::Applied); // what was final stays
    CHECK(status_of(hub, queued, "Brake") == TargetStatus::Queued); // the others are not touched
    CHECK(!hub.operation(queued)->done());
    for (const auto& peer : hub.peers())
    {
        CHECK_EQ(peer.connected, peer.participant == "Brake");
    }
    // A waiter gets its answer.
    CHECK(hub.wait_done(unsent, 0ms).has_value());
    // A result that still arrives changes nothing; a second end neither.
    hub.agent_result(engine.connection, CommandResult{queued, CommandStatus::Applied, at(20ms), false, {}});
    hub.agent_ended(engine.connection, "again");
    CHECK(status_of(hub, queued, "Engine") == TargetStatus::Unknown);
    CHECK(target(*hub.operation(queued), "Engine").reason == "the participant left");

    // The end of a connection that another connection of the same incarnation has replaced
    // settles nothing of the new one.
    const auto old_connection = brake.connection;
    brake.connect(std::vector<std::uint64_t>{queued});
    hub.agent_ended(old_connection, "the old connection ended");
    CHECK(status_of(hub, queued, "Brake") == TargetStatus::Queued);
    CHECK(hub.peers().at(0).connected);
    hub.agent_ended(brake.connection, "gone");
    CHECK(status_of(hub, queued, "Brake") == TargetStatus::Unknown);
    CHECK(hub.operation(queued)->done());
}

TEST_CASE(control_hub_reads_the_latest_values_of_several_peers_in_one_look)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    FakePeer body("Body", "b1", hub);
    FakePeer quiet("Quiet", "q1", hub);
    engine.connect();
    body.connect();
    quiet.connect();
    engine.step(at(20ms));
    body.step(at(30ms));

    // Without names: every peer, by name; a peer without a sample has its incarnation only.
    const auto all = hub.latest_of({});
    CHECK_EQ(all.size(), std::size_t{3});
    CHECK(all[0].participant == "Body" && all[0].incarnation == "b1");
    CHECK(all[1].participant == "Engine" && all[1].incarnation == "e1");
    CHECK(all[2].participant == "Quiet" && all[2].incarnation == "q1" && all[2].snapshot == nullptr);
    CHECK(all[0].snapshot != nullptr && all[0].snapshot->time == at(30ms));
    CHECK(all[1].snapshot != nullptr && all[1].snapshot->time == at(20ms));
    CHECK(all[1].snapshot == hub.latest("Engine")); // the sample itself, not a copy

    // With names: in the order given, a name once, and an entry for a name the hub does not know.
    const auto some = hub.latest_of({"Engine", "Nobody", "Engine", "Body"});
    CHECK_EQ(some.size(), std::size_t{3});
    CHECK(some[0].participant == "Engine" && some[0].incarnation == "e1" && some[0].snapshot != nullptr);
    CHECK(some[1].participant == "Nobody" && some[1].incarnation.empty() && some[1].snapshot == nullptr);
    CHECK(some[2].participant == "Body" && some[2].snapshot != nullptr);

    // none_after(): a sample at the time is not after it; a missing sample is none.
    CHECK(none_after(all, at(30ms)));
    CHECK(!none_after(all, at(30ms) - SimDuration{1}));
    CHECK(none_after(some, at(31ms)));
    CHECK(!none_after(some, at(25ms))); // Body's
    CHECK(none_after(hub.latest_of({"Engine", "Quiet", "Nobody"}), at(20ms)));
    CHECK(none_after({}, SimTime{}));
}

TEST_CASE(control_hub_readers_that_never_read_change_nothing_for_the_tap_and_the_writer)
{
    // The same steps and writes on a hub nobody reads and on one with many subscriptions that
    // are never polled (the smallest queues): the tap gets the same samples in the same order,
    // the writes get the same numbers and results, and no call of the agent side waits.
    const auto run = [](std::size_t readers, std::vector<std::string>& seen, std::vector<std::string>& results,
                        std::uint64_t& dropped) {
        Hub hub(HubOptions{.first_seq = 100});
        FakePeer engine("Engine", "e1", hub);
        FakePeer body("Body", "b1", hub);
        engine.connect();
        body.connect();
        hub.set_values_tap([&seen](const std::string& participant, const std::string&, const std::shared_ptr<const Snapshot>& sample) {
            seen.push_back(participant + "@" + std::to_string(sample->time.time_since_epoch().count() / 1'000'000) + "#" +
                           std::to_string(sample->sequence));
        });
        std::vector<std::shared_ptr<Subscription>> stuck;
        for (std::size_t i = 0; i < readers; ++i)
        {
            stuck.push_back(hub.subscribe(SubscriptionOptions{.capacity = 1 + i % 3}));
        }
        for (int i = 1; i <= 200; ++i)
        {
            const SimTime now = at(10ms * i);
            const auto seq = hub.write(OperationRequest{{PeerWrite{"Engine", "level", static_cast<double>(i % 100)}}, now});
            engine.step(now);
            if (i % 2 == 0)
            {
                body.step(now);
            }
            const auto op = hub.operation(seq);
            results.push_back(std::to_string(seq) + ":" + (op ? std::to_string(static_cast<int>(op->targets.at(0).status)) : "?") + "@" +
                              (op && op->targets.at(0).applied_at
                                   ? std::to_string(op->targets.at(0).applied_at->time_since_epoch().count()) : "-"));
        }
        hub.set_values_tap({});
        for (const auto& sub : stuck)
        {
            dropped += sub->dropped();
            // What a reader that never read holds is bounded: the peers, one sample each, and
            // its capacity of operations.
            CHECK(sub->poll(0, 0ms).size() <= std::size_t{2 + 2 + 3});
        }
    };
    std::vector<std::string> seenAlone, seenRead, resultsAlone, resultsRead;
    std::uint64_t droppedAlone = 0, droppedRead = 0;
    run(0, seenAlone, resultsAlone, droppedAlone);
    run(64, seenRead, resultsRead, droppedRead);
    CHECK_EQ(seenAlone.size(), std::size_t{300});
    CHECK(seenRead == seenAlone);
    CHECK(resultsRead == resultsAlone);
    CHECK(resultsAlone.front().rfind("100:", 0) == 0);
    CHECK(droppedRead > 0); // the readers lost updates; the hub did not wait for them
}

TEST_CASE(control_hub_tap_sees_every_sample_in_order_until_it_is_taken_away)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub);
    FakePeer body("Body", "b1", hub);
    engine.connect();
    body.connect();
    std::vector<std::string> seen;
    hub.set_values_tap([&seen](const std::string& participant, const std::string& incarnation,
                               const std::shared_ptr<const Snapshot>& sample) {
        seen.push_back(participant + "/" + incarnation + "@" + std::to_string(sample->time.time_since_epoch().count() / 1'000'000));
    });
    // A subscription keeps only the latest sample per peer; the tap gets each one.
    const auto subscription = hub.subscribe();
    engine.step(at(10ms));
    engine.step(at(20ms));
    body.step(at(20ms));
    engine.step(at(30ms));
    CHECK_EQ(seen.size(), std::size_t{4});
    CHECK(seen == (std::vector<std::string>{"Engine/e1@10", "Engine/e1@20", "Body/b1@20", "Engine/e1@30"}));
    hub.set_values_tap({});
    engine.step(at(40ms));
    CHECK_EQ(seen.size(), std::size_t{4});
}

namespace {

/// A peer that is a schema and a link, without an agent: the test says what its agent answers.
/// Its variables: the inputs "level" and "lamp", the parameter "gain" and the output "speed".
struct ScriptedPeer
{
    Hub& hub;
    std::string name;
    std::shared_ptr<FakePeer::Link> link = std::make_shared<FakePeer::Link>();
    ConnectionId connection = 0;

    ScriptedPeer(Hub& h, std::string n) : hub(h), name(std::move(n))
    {
        const auto variable = [](const char* variable_name, ValueType type, Causality causality) {
            VariableInfo v;
            v.name = variable_name;
            v.type = type;
            v.causality = causality;
            return v;
        };
        PeerHello hello{name, "i1",
                        {variable("level", ValueType::Real, Causality::Input), variable("lamp", ValueType::Bool, Causality::Input),
                         variable("gain", ValueType::Real, Causality::Parameter),
                         variable("speed", ValueType::Real, Causality::Output)},
                        64};
        connection = hub.agent_connected(std::move(hello), link);
    }

    /// Writes `names` (in this order, a value that fits each) as one command for the next step.
    std::uint64_t write(const std::vector<std::string>& names)
    {
        std::vector<PeerWrite> writes;
        for (const auto& variable : names)
        {
            writes.push_back({name, variable, variable == "lamp" ? Value{true} : Value{1.0}});
        }
        return hub.write(req(std::move(writes)));
    }
    void submitted(std::uint64_t seq, SubmitResult result) { hub.agent_submitted(connection, seq, result); }
    void queued(std::uint64_t seq) { submitted(seq, SubmitResult{SubmitStatus::Queued, {}}); }
    void result(std::uint64_t seq, CommandStatus status, std::string reason, std::optional<std::uint32_t> applied_writes)
    {
        hub.agent_result(connection, CommandResult{seq, status, std::nullopt, false, std::move(reason), applied_writes});
    }
    [[nodiscard]] TargetResult target(std::uint64_t seq) const { return ::target(*hub.operation(seq), name); }
};

using Count = std::optional<std::uint32_t>;
const Count kNone = std::nullopt;

/// The status and the two numbers of a target, as text: a difference shows all three.
std::string counts(const TargetResult& t)
{
    static const char* const kNames[] = {"Sent",     "Queued",    "Disconnected", "Applied", "Late",
                                         "Rejected", "Cancelled", "Invalid",      "Busy",    "Unknown"};
    const auto text = [](const Count& c) { return c ? std::to_string(*c) : std::string("-"); };
    return std::string(kNames[static_cast<std::size_t>(t.status)]) + " applied " + text(t.applied_writes) + " failed " +
           text(t.failed_write);
}

} // namespace

// TargetResult::applied_writes and failed_write for every status, with an agent that says them.
TEST_CASE(control_hub_says_how_many_writes_took_effect_and_which_one_was_refused)
{
    Hub hub;
    ScriptedPeer peer(hub, "Engine");
    const std::vector<std::string> three{"level", "lamp", "level"}; // a variable twice: the positions say which

    // Nothing is known while the command is on its way or queued.
    std::uint64_t seq = peer.write(three);
    CHECK(counts(peer.target(seq)) == "Sent applied - failed -");
    peer.queued(seq);
    CHECK(counts(peer.target(seq)) == "Queued applied - failed -");
    // Applied: all of them, whatever the agent's number is.
    peer.result(seq, CommandStatus::Applied, "", kNone);
    CHECK(counts(peer.target(seq)) == "Applied applied 3 failed -");
    // Late and Cancelled: none.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Late, "its time had passed", Count{0});
    CHECK(counts(peer.target(seq)) == "Late applied 0 failed -");
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Cancelled, "", Count{0});
    CHECK(counts(peer.target(seq)) == "Cancelled applied 0 failed -");

    // Rejected: the setter of the write at the agent's number threw, the ones before it took
    // effect. The second "level" is not confused with the first.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "level: a setter threw: no", Count{2});
    CHECK(counts(peer.target(seq)) == "Rejected applied 2 failed 2");
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "level: a setter threw: no", Count{0});
    CHECK(counts(peer.target(seq)) == "Rejected applied 0 failed 0");
    // The reason is not read: a name in it changes nothing.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "lamp: a setter threw: no", Count{2});
    CHECK(counts(peer.target(seq)) == "Rejected applied 2 failed 2");
    // A number the command does not have is not believed: more writes than there are.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "level: a setter threw: no", Count{4});
    CHECK(counts(peer.target(seq)) == "Rejected applied - failed -");
    // All of them and yet rejected: not believed either (a rejected command has a write that did
    // not take effect), and no write is the one.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "odd", Count{3});
    CHECK(counts(peer.target(seq)) == "Rejected applied - failed -");
    // A parameter and an input in one command, after the first step: nothing was applied, and no
    // setter was called, so no write is the one. With the parameter alone it is that write.
    seq = peer.write({"level", "gain"});
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "a parameter is writable only before the first step", Count{0});
    CHECK(counts(peer.target(seq)) == "Rejected applied 0 failed -");
    seq = peer.write({"gain"});
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "a parameter is writable only before the first step", Count{0});
    CHECK(counts(peer.target(seq)) == "Rejected applied 0 failed 0");
    // An agent that says nothing (a transport of its own that leaves the number out): only a
    // command of one write is known, and the reason is not read for a name.
    seq = peer.write(three);
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "level: a setter threw: no", kNone);
    CHECK(counts(peer.target(seq)) == "Rejected applied - failed -");
    seq = peer.write({"lamp"});
    peer.queued(seq);
    peer.result(seq, CommandStatus::Rejected, "lamp: a setter threw: no", kNone);
    CHECK(counts(peer.target(seq)) == "Rejected applied 0 failed 0");

    // Invalid, refused by the agent's check: none applied, and the write the agent names.
    seq = peer.write(three);
    peer.submitted(seq, SubmitResult{SubmitStatus::Invalid, "level: out of range", Count{2}});
    CHECK(counts(peer.target(seq)) == "Invalid applied 0 failed 2");
    seq = peer.write(three);
    peer.submitted(seq, SubmitResult{SubmitStatus::Invalid, "level: out of range", Count{3}}); // not a write of the command
    CHECK(counts(peer.target(seq)) == "Invalid applied 0 failed -");
    seq = peer.write(three);
    peer.submitted(seq, SubmitResult{SubmitStatus::Invalid, "level: out of range", kNone}); // the reason is not read
    CHECK(counts(peer.target(seq)) == "Invalid applied 0 failed -");
    // Busy, said by the agent.
    seq = peer.write(three);
    peer.submitted(seq, SubmitResult{SubmitStatus::Busy, "full"});
    CHECK(counts(peer.target(seq)) == "Busy applied 0 failed -");

    // Invalid, refused by the hub's own check: the write at which it stopped.
    CHECK(counts(peer.target(peer.write({"level", "lamp", "speed", "nope"}))) == "Invalid applied 0 failed 2"); // an output
    CHECK(counts(peer.target(peer.write({"nope", "level"}))) == "Invalid applied 0 failed 0");                 // unknown
    // ... or the command as a whole: no write is the one.
    CHECK(counts(::target(*hub.operation(hub.write(req({{"Nobody", "level", 1.0}}))), "Nobody")) == "Invalid applied 0 failed -");
    peer.link->up = false;
    CHECK(counts(peer.target(peer.write(three))) == "Invalid applied 0 failed -"); // the link refused it
    peer.link->up = true;

    // No result will come: not known.
    seq = peer.write(three);
    peer.queued(seq);
    hub.agent_disconnected(peer.connection);
    CHECK(counts(peer.target(seq)) == "Disconnected applied - failed -");
    CHECK(hub.forget_peer("Engine"));
    CHECK(counts(peer.target(seq)) == "Unknown applied - failed -");
}

// The same with a variable map: the positions are those of the request, whatever the names are.
TEST_CASE(control_hub_positions_of_writes_do_not_depend_on_the_variable_map)
{
    Hub hub;
    hub.set_variable_map("Engine", VariableMap{VariableMapping{.name = "throttle", .from = "level"}});
    ScriptedPeer peer(hub, "Engine");
    std::uint64_t seq = peer.write({"lamp", "throttle"});
    peer.submitted(seq, SubmitResult{SubmitStatus::Invalid, "level: out of range", Count{1}});
    CHECK(counts(peer.target(seq)) == "Invalid applied 0 failed 1");
    CHECK(peer.target(seq).reason == "throttle (the executable's level): out of range");
    // The executable's name is not a name to write under the map: the hub's check stops there.
    CHECK(counts(peer.target(peer.write({"lamp", "level"}))) == "Invalid applied 0 failed 1");
}

namespace {

/// The AvailabilityUpdates among `updates`, in order.
std::vector<AvailabilityUpdate> availability_of(const std::vector<Update>& updates)
{
    std::vector<AvailabilityUpdate> out;
    for (const auto& u : updates)
    {
        if (const auto* a = std::get_if<AvailabilityUpdate>(&u))
        {
            out.push_back(*a);
        }
    }
    return out;
}

using States = std::vector<Availability>;

} // namespace

TEST_CASE(control_hub_passes_the_availability_on_and_decides_nothing_by_it)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub, 256, true), body("Body", "b1", hub);
    auto plain = hub.subscribe();
    auto sub = hub.subscribe(SubscriptionOptions{.availability = true});
    engine.connect();
    body.connect();
    // The peers first, then the availability of the one that reports, as declared.
    auto updates = sub->poll(0, 0ms);
    CHECK_EQ(updates.size(), std::size_t{3});
    CHECK(std::holds_alternative<PeerUpdate>(updates.at(0)));
    CHECK(std::holds_alternative<PeerUpdate>(updates.at(1)));
    auto got = availability_of(updates);
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).peer == "Engine");
    CHECK(got.at(0).incarnation == "e1");
    CHECK(got.at(0).states == (States{Availability::Unknown, Availability::Active, Availability::Inactive}));

    // A write to an Inactive variable is routed and applied as any other.
    engine.step(at(0ms));
    engine.agent->set_availability("level", Availability::Inactive);
    engine.report_availability();
    const auto seq = hub.write(req({{"Engine", "level", 40.0}}));
    engine.step(at(10ms));
    CHECK(status_of(hub, seq, "Engine") == TargetStatus::Applied);
    CHECK_EQ(engine.level, 40.0);
    got = availability_of(sub->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).states == (States{Availability::Inactive, Availability::Active, Availability::Inactive}));

    // A subscriber that does not read keeps the latest per peer, and drops nothing.
    for (int i = 0; i < 100; ++i)
    {
        engine.agent->set_availability("level", i % 2 == 0 ? Availability::Active : Availability::Inactive);
        engine.agent->set_availability("speed", Availability::Active);
        engine.report_availability();
    }
    got = availability_of(sub->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).states == (States{Availability::Inactive, Availability::Active, Availability::Active}));
    CHECK_EQ(sub->dropped(), std::uint64_t{0});

    // Ignored: an older version, the same states again, a list of another size, another connection.
    const std::uint64_t now = engine.agent->availability_version();
    const States other{Availability::Unknown, Availability::Unknown, Availability::Unknown};
    hub.agent_availability(engine.connection, now - 1, other);
    hub.agent_availability(engine.connection, now, got.at(0).states);
    hub.agent_availability(engine.connection, now + 1, States{Availability::Unknown});
    hub.agent_availability(engine.connection + 100, now + 1, other);
    hub.agent_availability(body.connection, 1, other); // nothing of Body reports
    CHECK(availability_of(sub->poll(0, 0ms)).empty());
    // A new subscription begins with what the hub has now.
    got = availability_of(hub.subscribe(SubscriptionOptions{.availability = true})->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).states == (States{Availability::Inactive, Availability::Active, Availability::Active}));

    // Not asked for: none.
    CHECK(availability_of(plain->poll(0, 0ms)).empty());
}

TEST_CASE(control_hub_a_new_incarnation_starts_from_the_declared_availability)
{
    Hub hub;
    FakePeer engine("Engine", "e1", hub, 256, true);
    engine.connect();
    engine.step(at(0ms));
    engine.agent->set_availability("level", Availability::Active);
    engine.agent->set_availability("speed", Availability::Active);
    engine.report_availability();
    auto sub = hub.subscribe(SubscriptionOptions{.availability = true});
    // The same process again keeps its states.
    engine.connect();
    auto got = availability_of(sub->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).states == (States{Availability::Active, Availability::Active, Availability::Active}));
    engine.agent->set_availability("level", Availability::Inactive);
    engine.report_availability(); // unread when the peer restarts

    // Another process: what the subscription held of the one before is replaced by the declared
    // states of the new one, and what the old connection says is ignored.
    FakePeer restarted("Engine", "e2", hub, 256, true);
    restarted.connect();
    engine.report_availability();
    got = availability_of(sub->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).incarnation == "e2");
    CHECK(got.at(0).states == (States{Availability::Unknown, Availability::Active, Availability::Inactive}));
    // The new process counts its versions from 0 again.
    restarted.step(at(0ms));
    restarted.agent->set_availability("speed", Availability::Active);
    restarted.report_availability();
    got = availability_of(sub->poll(0, 0ms));
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(got.at(0).states == (States{Availability::Unknown, Availability::Active, Availability::Active}));
    // A forgotten peer takes its availability with it.
    restarted.agent->set_availability("speed", Availability::Inactive);
    restarted.report_availability();
    hub.agent_disconnected(restarted.connection);
    hub.forget_peer("Engine");
    CHECK(availability_of(sub->poll(0, 0ms)).empty());
}

TEST_CASE(control_hub_availability_follows_the_variable_map_by_position)
{
    Hub hub;
    hub.set_variable_map("Engine", VariableMap{VariableMapping{.name = "throttle", .from = "level"},
                                               VariableMapping{.name = "aa.speed", .from = "speed"}});
    FakePeer engine("Engine", "e1", hub, 256, true);
    engine.connect();
    auto sub = hub.subscribe(SubscriptionOptions{.availability = true});
    const auto updates = sub->poll(0, 0ms);
    const auto got = availability_of(updates);
    CHECK_EQ(got.size(), std::size_t{1});
    const PeerInfo& seen = std::get<PeerUpdate>(updates.at(0)).peer;
    CHECK_EQ(seen.schema.size(), got.at(0).states.size());
    CHECK(seen.schema.at(0).name == "throttle");
    CHECK(seen.schema.at(0).availability == std::optional(Availability::Unknown));
    CHECK(got.at(0).states.at(0) == Availability::Unknown);
    CHECK(seen.schema.at(2).name == "aa.speed");
    CHECK(got.at(0).states.at(2) == Availability::Inactive);
}

namespace {

/// The number of `peer`'s commands in flight, computed from the public view only: the targets
/// that are not final in the operations that are not done. `open` holds the seqs that were not
/// done when last looked at (an operation that is gone had finished).
std::size_t in_flight(Hub& hub, std::vector<std::uint64_t>& open, const std::string& peer)
{
    std::size_t n = 0;
    std::vector<std::uint64_t> still;
    for (const auto seq : open)
    {
        const auto op = hub.operation(seq);
        if (!op || op->done())
        {
            continue;
        }
        still.push_back(seq);
        for (const auto& t : op->targets)
        {
            n += t.peer == peer && !is_final(t.status) ? 1 : 0;
        }
    }
    open = std::move(still);
    return n;
}

bool is_connected(Hub& hub, const std::string& peer)
{
    for (const auto& p : hub.peers())
    {
        if (p.participant == peer)
        {
            return p.connected;
        }
    }
    return false;
}

/// Writes four commands to Engine (its limit), so that the next one is turned away.
void fill_to_the_limit(Hub& hub)
{
    for (int i = 0; i < 4; ++i)
    {
        CHECK(status_of(hub, hub.write(req({{"Engine", "level", 1.0 + i}}, at(1s))), "Engine") == TargetStatus::Sent);
    }
}

TargetStatus next_write(Hub& hub)
{
    return status_of(hub, hub.write(req({{"Engine", "lamp", true}}, at(2s))), "Engine");
}

} // namespace

TEST_CASE(control_hub_busy_decision_equals_the_count_of_open_targets_in_any_sequence)
{
    // Whatever happened before (answers, refusals, a failing link, disconnects, reconnects,
    // restarts, an ended agent, a forgotten peer, the oldest finished operations dropped), a write
    // is turned away as Busy exactly when the peer has max_pending targets that are not final in
    // operations that are not done.
    std::mt19937_64 rng(20261119);
    Hub hub(HubOptions{.max_operations = 8});
    const std::vector<std::string> names{"Engine", "Body"};
    std::vector<std::unique_ptr<FakePeer>> peers;
    std::vector<int> incarnation(2, 1);
    for (std::size_t i = 0; i < 2; ++i)
    {
        peers.push_back(std::make_unique<FakePeer>(names[i], names[i] + "1", hub));
        peers[i]->connect();
    }
    std::vector<std::uint64_t> open;
    SimDuration now = 0ms;
    std::size_t busy = 0, not_busy = 0;
    const auto roll = [&](std::uint64_t n) { return static_cast<std::size_t>(rng() % n); };

    for (int iteration = 0; iteration < 4000; ++iteration)
    {
        const auto which = roll(2);
        auto& peer = *peers[which];
        const auto action = roll(14);
        if (action < 7) // a write to one peer or to both; sometimes with a value out of range
        {
            const bool bad = roll(8) == 0;
            std::vector<PeerWrite> writes;
            std::vector<bool> valid;
            for (std::size_t i = 0; i < 2; ++i)
            {
                if (i == which || roll(4) == 0)
                {
                    const bool lamp = roll(2) == 0;
                    writes.push_back({names[i], lamp ? "lamp" : "level", lamp ? Value{true} : Value{bad ? 500.0 : 50.0}});
                    valid.push_back(lamp || !bad);
                }
            }
            std::vector<std::size_t> before;
            std::vector<bool> connected;
            for (const auto& w : writes)
            {
                before.push_back(in_flight(hub, open, w.peer));
                connected.push_back(is_connected(hub, w.peer));
            }
            const auto when = roll(3) == 0 ? std::optional<SimTime>{} : std::optional<SimTime>(at(now + std::chrono::milliseconds(roll(60))));
            const auto seq = hub.write(req(writes, when));
            open.push_back(seq);
            const auto result = *hub.operation(seq);
            for (std::size_t i = 0; i < writes.size(); ++i)
            {
                const auto status = target(result, writes[i].peer).status;
                if (status == TargetStatus::Busy)
                {
                    CHECK(before[i] >= 4);
                    ++busy;
                }
                else if (connected[i] && valid[i])
                {
                    CHECK(before[i] < 4); // nothing else turns it away
                    ++not_busy;
                }
            }
        }
        else if (action < 9) // the transport delivers, the agent steps and answers
        {
            now += 10ms;
            peer.step(at(now), roll(4) != 0);
        }
        else if (action == 9) // the link fails for a while, or works again
        {
            peer.link->up = roll(2) == 0;
        }
        else if (action == 10) // the connection is lost; the same process may come back
        {
            hub.agent_disconnected(peer.connection);
            if (roll(2) == 0)
            {
                peer.link->up = true;
                peer.connect();
            }
        }
        else if (action == 11) // a restart: another incarnation
        {
            peers[which] = std::make_unique<FakePeer>(names[which], names[which] + std::to_string(++incarnation[which]), hub);
            peers[which]->connect();
        }
        else if (action == 12) // the agent ends, and perhaps connects again
        {
            hub.agent_ended(peer.connection, "ended in the test");
            if (roll(2) == 0)
            {
                peer.link->up = true;
                peer.connect();
            }
        }
        else // the peer is forgotten, and perhaps comes back
        {
            hub.agent_disconnected(peer.connection);
            hub.forget_peer(peer.name);
            if (roll(2) == 0)
            {
                peer.link->up = true;
                peer.connect();
            }
        }
    }
    // Both outcomes must have been reached many times, or the run proves nothing.
    CHECK(busy > 50);
    CHECK(not_busy > 200);
}

TEST_CASE(control_hub_open_commands_free_up_on_every_way_an_operation_ends)
{
    // (a) the agent answers
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub);
        engine.connect();
        fill_to_the_limit(hub);
        CHECK(next_write(hub) == TargetStatus::Busy);
        for (int i = 0; i < 5; ++i)
        {
            engine.step(at(1s + std::chrono::milliseconds(i)));
        }
        CHECK(next_write(hub) == TargetStatus::Sent);
    }
    // (b) the agent turns commands away (it holds one at a time)
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub, 1);
        engine.connect();
        fill_to_the_limit(hub);
        CHECK(next_write(hub) == TargetStatus::Busy);
        engine.pump();
        CHECK(next_write(hub) == TargetStatus::Sent); // three of the four were refused by the agent
        CHECK(next_write(hub) == TargetStatus::Sent);
        CHECK(next_write(hub) == TargetStatus::Sent);
        CHECK(next_write(hub) == TargetStatus::Busy);
    }
    // (c) a disconnect keeps them open; after the reconnect the results free them
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub);
        engine.connect();
        fill_to_the_limit(hub);
        engine.pump();
        hub.agent_disconnected(engine.connection);
        engine.connect();
        CHECK(next_write(hub) == TargetStatus::Busy);
        for (int i = 0; i < 5; ++i)
        {
            engine.step(at(1s + std::chrono::milliseconds(i)));
        }
        CHECK(next_write(hub) == TargetStatus::Sent);
    }
    // (d) a restart settles them
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub);
        engine.connect();
        fill_to_the_limit(hub);
        engine.pump();
        CHECK(next_write(hub) == TargetStatus::Busy);
        FakePeer restarted("Engine", "e2", hub);
        restarted.connect();
        CHECK(next_write(hub) == TargetStatus::Sent);
    }
    // (e) the end of the agent settles them
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub);
        engine.connect();
        fill_to_the_limit(hub);
        engine.pump();
        CHECK(next_write(hub) == TargetStatus::Busy);
        hub.agent_ended(engine.connection, "ended in the test");
        engine.connect();
        CHECK(next_write(hub) == TargetStatus::Sent);
    }
    // (f) forgetting the peer settles them
    {
        Hub hub;
        FakePeer engine("Engine", "e1", hub);
        engine.connect();
        fill_to_the_limit(hub);
        CHECK(next_write(hub) == TargetStatus::Busy);
        hub.agent_disconnected(engine.connection);
        CHECK(hub.forget_peer("Engine"));
        engine.connect();
        CHECK(next_write(hub) == TargetStatus::Sent);
    }
}

TEST_CASE(control_hub_busy_decision_does_not_depend_on_the_finished_operations_kept)
{
    Hub hub(HubOptions{.max_operations = 2});
    FakePeer engine("Engine", "e1", hub);
    engine.connect();
    for (int round = 0; round < 20; ++round)
    {
        const auto t = std::chrono::milliseconds(round * 10);
        for (int i = 0; i < 4; ++i)
        {
            CHECK(status_of(hub, hub.write(req({{"Engine", "level", 1.0 + i}}, at(t + 5ms))), "Engine") == TargetStatus::Sent);
        }
        CHECK(next_write(hub) == TargetStatus::Busy);
        engine.step(at(t + 5ms));
        engine.step(at(t + 6ms));
        engine.step(at(t + 7ms));
        engine.step(at(t + 8ms));
        engine.step(at(t + 9ms));
    }
}
