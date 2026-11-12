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
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
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

    FakePeer(std::string n, std::string inc, Hub& h, std::size_t max_pending = 256) :
      name(std::move(n)), incarnation(std::move(inc)), hub(h)
    {
        agent = std::make_unique<Agent>(AgentOptions{.max_pending = max_pending, .sample_period = SimDuration::zero()});
        agent->input("level", &level, {.unit = "%", .min = 0.0, .max = 100.0});
        agent->input("lamp", &lamp);
        agent->output("speed", &speed);
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
    const auto queued = hub.wait_queued(seq, 5s);
    transport.join();
    CHECK(queued.has_value());
    CHECK(queued->targets.at(0).status == TargetStatus::Queued);
    engine.step(at(30ms));
    // The result comes with the wait, even if the operation is forgotten right after.
    const auto done = hub.wait_done(seq, 5s);
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
    const auto w = hub.wait_queued(mixed, 1s);
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
                const auto done = hub.wait_done(seq, 10s);
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
        const auto deadline = std::chrono::steady_clock::now() + 2s;
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
