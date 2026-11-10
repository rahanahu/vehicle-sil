// SPDX-License-Identifier: MIT
//
// SimulationMonitor (one reader of the simulation for the whole server) with a scripted
// TimeControl, and what Watch and ListPeers take from it.
#include "test_harness.hpp"

#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/server.hpp"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
namespace v1 = vehicle_sil::control::v1;
using control_grpc::SimulationMonitor;
using control_grpc::SimulationMonitorOptions;
using control_grpc::SimulationSnapshot;

control_grpc::TimeView available()
{
    control_grpc::TimeView v;
    v.available = true;
    return v;
}

/// A time control whose answers the test sets; counts how often it is read.
class ScriptedTime final : public control_grpc::TimeControl
{
public:
    mutable std::mutex mutex;
    control_grpc::TimeView view = available();
    std::vector<std::pair<std::string, std::string>> members{{"Engine", ""}, {"Body", ""}};
    std::optional<LeftParticipant> left;
    std::string participants_fail; ///< not empty: participants() throws this
    mutable std::atomic<int> state_reads{0};
    mutable std::atomic<int> participant_reads{0};

    template <typename F>
    void change(F&& f)
    {
        const std::lock_guard lock(mutex);
        f(*this);
    }

    [[nodiscard]] control_grpc::TimeView state() const override
    {
        ++state_reads;
        const std::lock_guard lock(mutex);
        return view;
    }
    void pause(std::optional<SimTime>) override {}
    void resume() override {}
    void run_until(SimTime) override {}
    void step(SimDuration) override {}
    void set_speed(std::optional<double>) override {}
    std::optional<SimTime> wait_paused(std::chrono::milliseconds) override { return std::nullopt; }
    void stop(const std::string&) override {}
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> participants() const override
    {
        ++participant_reads;
        const std::lock_guard lock(mutex);
        if (!participants_fail.empty())
        {
            throw std::runtime_error(participants_fail);
        }
        return members;
    }
    [[nodiscard]] std::optional<LeftParticipant> left_participant() const override
    {
        const std::lock_guard lock(mutex);
        return left;
    }
};

/// What a subscriber was told.
struct Seen
{
    std::mutex mutex;
    std::vector<SimulationSnapshot> snapshots;

    SimulationMonitor::Callback callback()
    {
        return [this](const SimulationSnapshot& s) {
            const std::lock_guard lock(mutex);
            snapshots.push_back(s);
        };
    }
    std::size_t size()
    {
        const std::lock_guard lock(mutex);
        return snapshots.size();
    }
    SimulationSnapshot last()
    {
        const std::lock_guard lock(mutex);
        return snapshots.back();
    }
};

/// A monitor without a thread, on a clock the test moves.
struct ManualMonitor
{
    std::chrono::steady_clock::time_point now{};
    SimulationMonitor monitor;

    SimulationMonitorOptions options()
    {
        SimulationMonitorOptions o;
        o.period = 0ms;
        o.leave_grace = 5s;
        o.clock = [this] { return now; };
        return o;
    }

    explicit ManualMonitor(ScriptedTime& time) :
      monitor(time, options())
    {
    }
};

std::shared_ptr<grpc::Channel> channel(const control_grpc::ControlServer& server)
{
    return grpc::CreateChannel("127.0.0.1:" + std::to_string(server.port()), grpc::InsecureChannelCredentials());
}

/// A Watch stream read by the test.
struct WatchStream
{
    grpc::ClientContext context;
    std::unique_ptr<grpc::ClientReader<v1::Update>> reader;

    WatchStream(v1::ControlService::Stub& stub, bool time, bool simulation)
    {
        context.set_deadline(std::chrono::system_clock::now() + 30s);
        v1::WatchRequest request;
        request.set_time(time);
        request.set_simulation(simulation);
        reader = stub.Watch(&context, request);
    }

    /// The updates up to and including initial_done.
    std::vector<v1::Update> initial()
    {
        std::vector<v1::Update> out;
        v1::Update u;
        while (reader->Read(&u))
        {
            out.push_back(u);
            if (u.kind_case() == v1::Update::kInitialDone)
            {
                break;
            }
        }
        return out;
    }

    /// The next update of `kind`.
    std::optional<v1::Update> next(v1::Update::KindCase kind)
    {
        v1::Update u;
        while (reader->Read(&u))
        {
            if (u.kind_case() == kind)
            {
                return u;
            }
        }
        return std::nullopt;
    }
};

std::size_t count(const std::vector<v1::Update>& updates, v1::Update::KindCase kind)
{
    std::size_t n = 0;
    for (const auto& u : updates)
    {
        n += u.kind_case() == kind ? 1 : 0;
    }
    return n;
}

} // namespace

TEST_CASE(simulation_monitor_tells_nothing_while_nothing_changes)
{
    ScriptedTime time;
    ManualMonitor m(time);
    Seen seen;
    auto sub = m.monitor.subscribe(seen.callback());
    const auto first = m.monitor.snapshot();
    CHECK_EQ(first.version, 1U);
    CHECK(first.participants.size() == 2);
    for (int i = 0; i < 5; ++i)
    {
        m.now += 1s;
        m.monitor.poll();
    }
    CHECK_EQ(seen.size(), 0U);
    CHECK_EQ(m.monitor.snapshot().version, 1U);
}

TEST_CASE(simulation_monitor_tells_each_change_once)
{
    ScriptedTime time;
    ManualMonitor m(time);
    Seen seen;
    auto sub = m.monitor.subscribe(seen.callback());

    // The time.
    time.change([](ScriptedTime& t) { t.view.paused = true; });
    m.monitor.poll();
    m.monitor.poll();
    CHECK_EQ(seen.size(), 1U);
    CHECK(seen.last().time.paused);

    // A participant's state.
    time.change([](ScriptedTime& t) { t.members[0].second = "Running"; });
    m.monitor.poll();
    m.monitor.poll();
    CHECK_EQ(seen.size(), 2U);
    CHECK(seen.last().state_of("Engine") == "Running");

    // A failure: Error at once, with the name, the state and the reason; it stays.
    time.change([](ScriptedTime& t) {
        t.members[1].second = "Error";
        t.left = LeftParticipant{"Body", "Error", true};
    });
    m.monitor.poll();
    CHECK_EQ(seen.size(), 3U);
    CHECK(seen.last().failure.has_value());
    CHECK(seen.last().failure->participant == "Body");
    CHECK(seen.last().failure->reason.find("state Error") != std::string::npos);
    time.change([](ScriptedTime& t) { t.left.reset(); }); // aborted: nobody is "left" any more
    m.monitor.poll();
    CHECK_EQ(seen.size(), 3U);
    CHECK(m.monitor.snapshot().failure.has_value());

    // The end.
    time.change([](ScriptedTime& t) {
        t.view.paused = false;
        t.view.ended = true;
    });
    m.monitor.poll();
    m.monitor.poll();
    CHECK_EQ(seen.size(), 4U);
    CHECK(seen.last().ended);
    CHECK_EQ(seen.last().version, 5U);
}

TEST_CASE(simulation_monitor_fails_a_stopped_participant_only_after_the_grace)
{
    ScriptedTime time;
    ManualMonitor m(time);
    Seen seen;
    auto sub = m.monitor.subscribe(seen.callback());
    time.change([](ScriptedTime& t) { t.left = LeftParticipant{"Body", "Stopped", false}; });
    m.monitor.poll();
    m.now += 4s;
    m.monitor.poll();
    CHECK_EQ(seen.size(), 0U);
    m.now += 1s;
    m.monitor.poll();
    CHECK_EQ(seen.size(), 1U);
    CHECK(seen.last().failure.has_value());
    CHECK(seen.last().failure->state == "Stopped");
}

TEST_CASE(simulation_monitor_keeps_the_last_participants_when_it_cannot_read_them)
{
    ScriptedTime time;
    time.change([](ScriptedTime& t) { t.members[0].second = "Running"; });
    ManualMonitor m(time);
    Seen seen;
    auto sub = m.monitor.subscribe(seen.callback());
    time.change([](ScriptedTime& t) { t.participants_fail = "the registry is gone"; });
    m.monitor.poll();
    CHECK_EQ(seen.size(), 1U);
    CHECK(seen.last().participants_error == "the registry is gone");
    CHECK(seen.last().state_of("Engine") == "Running");
    time.change([](ScriptedTime& t) { t.participants_fail.clear(); });
    m.monitor.poll();
    CHECK_EQ(seen.size(), 2U);
    CHECK(seen.last().participants_error.empty());
}

TEST_CASE(simulation_monitor_stops_calling_a_subscriber_once_it_is_reset)
{
    ScriptedTime time;
    ManualMonitor m(time);
    Seen a;
    Seen b;
    auto sub_a = m.monitor.subscribe(a.callback());
    auto sub_b = m.monitor.subscribe(b.callback());
    time.change([](ScriptedTime& t) { t.view.now = SimTime{} + 10ms; });
    m.monitor.poll();
    sub_a.reset();
    time.change([](ScriptedTime& t) { t.view.now = SimTime{} + 20ms; });
    m.monitor.poll();
    CHECK_EQ(a.size(), 1U);
    CHECK_EQ(b.size(), 2U);
    SimulationMonitor::Subscription moved = std::move(sub_b);
    moved.reset();
    time.change([](ScriptedTime& t) { t.view.now = SimTime{} + 30ms; });
    m.monitor.poll();
    CHECK_EQ(b.size(), 2U);
}

TEST_CASE(simulation_monitor_thread_reads_on_its_own_and_unsubscribes_while_it_runs)
{
    ScriptedTime time;
    SimulationMonitorOptions options;
    options.period = 1ms;
    SimulationMonitor monitor(time, options);
    std::atomic<int> calls{0};
    for (int i = 0; i < 50; ++i)
    {
        time.change([i](ScriptedTime& t) { t.view.now = SimTime{} + std::chrono::milliseconds(i); });
        auto sub = monitor.subscribe([&calls](const SimulationSnapshot&) { ++calls; });
        std::this_thread::sleep_for(1ms);
    }
    // The thread reads without poll(): the last time arrives.
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (monitor.snapshot().time.now != SimTime{} + 49ms && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(1ms);
    }
    CHECK(monitor.snapshot().time.now == SimTime{} + 49ms);
}

TEST_CASE(control_server_watches_share_one_reader_of_the_time)
{
    ScriptedTime time;
    control_grpc::ServerOptions options;
    options.time = &time;
    options.monitor_period = 0ms; // only poll() reads
    control_grpc::ControlServer server(options);
    const int reads = time.state_reads.load();
    auto stub = v1::ControlService::NewStub(channel(server));
    WatchStream a(*stub, true, true);
    WatchStream b(*stub, true, true);
    const auto initial_a = a.initial();
    const auto initial_b = b.initial();
    CHECK_EQ(count(initial_a, v1::Update::kTime), 1U);
    CHECK_EQ(count(initial_a, v1::Update::kSimulation), 1U);
    CHECK_EQ(count(initial_b, v1::Update::kSimulation), 1U);
    // Several of their waits pass: the watches read the monitor, not the time control.
    std::this_thread::sleep_for(350ms);
    CHECK_EQ(time.state_reads.load(), reads);

    // One read of the monitor reaches both watches.
    time.change([](ScriptedTime& t) {
        t.members[0].second = "Error";
        t.left = LeftParticipant{"Engine", "Error", true};
    });
    server.simulation()->poll();
    CHECK_EQ(time.state_reads.load(), reads + 1);
    for (auto* w : {&a, &b})
    {
        const auto u = w->next(v1::Update::kSimulation);
        CHECK(u.has_value());
        CHECK(u->simulation().available());
        CHECK(u->simulation().failed_participant() == "Engine");
        CHECK(u->simulation().failed_state() == "Error");
        CHECK(u->simulation().failure_reason().find("left the simulation (state Error)") == 0);
        CHECK(u->simulation().participants_size() == 2);
        CHECK(u->simulation().participants(0).state() == "Error");
    }
    a.context.TryCancel();
    b.context.TryCancel();
}

TEST_CASE(control_server_watch_says_when_it_coordinates_no_simulation)
{
    control_grpc::ControlServer server(control_grpc::ServerOptions{});
    CHECK(server.simulation() == nullptr);
    auto stub = v1::ControlService::NewStub(channel(server));
    WatchStream w(*stub, true, true);
    const auto initial = w.initial();
    CHECK_EQ(count(initial, v1::Update::kTime), 0U);
    CHECK_EQ(count(initial, v1::Update::kSimulation), 1U);
    for (const auto& u : initial)
    {
        if (u.kind_case() == v1::Update::kSimulation)
        {
            CHECK(!u.simulation().available());
        }
    }
    w.context.TryCancel();
}

TEST_CASE(control_server_list_peers_says_when_it_cannot_read_the_participants)
{
    ScriptedTime time;
    time.change([](ScriptedTime& t) { t.members[0].second = "Running"; });
    control_grpc::ServerOptions options;
    options.time = &time;
    options.monitor_period = 0ms;
    control_grpc::ControlServer server(options);
    auto stub = v1::ControlService::NewStub(channel(server));
    {
        grpc::ClientContext context;
        v1::ListPeersReply reply;
        CHECK(stub->ListPeers(&context, v1::ListPeersRequest{}, &reply).ok());
        CHECK(reply.simulation_error().empty());
        CHECK_EQ(reply.peers_size(), 2);
    }
    time.change([](ScriptedTime& t) { t.participants_fail = "the registry is gone"; });
    server.simulation()->poll();
    grpc::ClientContext context;
    v1::ListPeersReply reply;
    CHECK(stub->ListPeers(&context, v1::ListPeersRequest{}, &reply).ok());
    CHECK(reply.simulation_error().find("the registry is gone") != std::string::npos);
    CHECK_EQ(reply.peers_size(), 2); // the last known participants, not an empty list
    CHECK(reply.peers(0).simulation_state() == "Running");
}

TEST_CASE(control_server_shuts_down_with_a_watch_of_the_simulation_open)
{
    ScriptedTime time;
    control_grpc::ServerOptions options;
    options.time = &time;
    options.monitor_period = 1ms;
    auto server = std::make_unique<control_grpc::ControlServer>(options);
    auto stub = v1::ControlService::NewStub(channel(*server));
    WatchStream w(*stub, true, true);
    CHECK(!w.initial().empty());
    std::atomic<bool> stop{false};
    std::thread changer([&] {
        for (int i = 0; !stop.load(); ++i)
        {
            time.change([i](ScriptedTime& t) { t.view.now = SimTime{} + std::chrono::milliseconds(i); });
            std::this_thread::sleep_for(1ms);
        }
    });
    CHECK(w.next(v1::Update::kTime).has_value());
    server->shutdown(); // the watch is still open
    server.reset();
    stop = true;
    changer.join();
    v1::Update u;
    while (w.reader->Read(&u))
    {
    }
    CHECK(true);
}
