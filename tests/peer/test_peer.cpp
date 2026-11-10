// SPDX-License-Identifier: MIT
//
// vehicle_sil::Peer on real SIL Kit participants (in-process registry): J1939 ECUs, LIN master
// and slave, J1939 and LIN in one participant, the same results as the local virtual bus,
// real time with an external port, set-up errors, failures and the order of destruction; the
// ECU application of examples/j1939/ecu_pair on two participants.
#include "ecu_pair.hpp"
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/real_time_clock.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/testkit/local_can.hpp"
#include "vehicle_sil/testkit/recorders.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::LifecycleMode;
using vehicle_sil::Peer;
using vehicle_sil::PeerOptions;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::SimulationController;
using vehicle_sil::SimulationControllerOptions;
using vehicle_sil::VirtualTimeOptions;
using vehicle_sil::unit::InProcessRegistry;
using vehicle_sil::unit::throws;
namespace can = vehicle_sil::can;
namespace j1939 = vehicle_sil::j1939;
namespace lin = vehicle_sil::lin;
namespace orch = SilKit::Services::Orchestration;
using j1939::EventKind;

namespace {

constexpr j1939::Pgn kPgnEngineSpeed{0xFF10}; ///< proprietary B: the engine's broadcast
constexpr j1939::Pgn kPgnSoftwareId{0xFEDA};
constexpr lin::LinId kCommandId = 0x10; ///< published by the master: the value from J1939
constexpr lin::LinId kDoorId = 0x20;    ///< published by the slave: the door state

SimTime at(std::chrono::nanoseconds t) { return SimTime{} + t; }
std::int64_t ms(SimTime t) { return (t - SimTime{}) / 1ms; }

PeerOptions coordinated(std::string name, const std::string& uri, SimDuration step)
{
    PeerOptions o;
    o.participant = std::move(name);
    o.registry_uri = uri;
    o.lifecycle = LifecycleMode::Coordinated;
    o.virtual_time = VirtualTimeOptions{step, std::nullopt, std::nullopt};
    return o;
}

/// What a receiver saw before 500 ms, as text.
struct Seen
{
    std::vector<std::string> lines;
    void add(const j1939::Message& m)
    {
        if (m.time >= at(500ms))
        {
            return; // the runs end at slightly different steps
        }
        lines.push_back(std::to_string(m.pgn.value()) + " " + std::to_string(m.source) + " " + std::to_string(ms(m.time)) + " " +
                        std::to_string(m.data.empty() ? -1 : m.data[0]));
    }
};

} // namespace

TEST_CASE(peer_runs_j1939_and_lin_nodes_in_three_participants)
{
    InProcessRegistry registry;
    SimulationControllerOptions control;
    control.registry_uri = registry.uri;
    control.required_participants = {"Engine", "Body", "Door"};
    control.stop_at = 1s;
    SimulationController controller(control);

    // Models first: they outlive the peers whose handlers use them.
    std::uint8_t engineCounter = 0;
    std::uint8_t lastSpeed = 0;
    std::vector<lin::LinScheduleResult> bodyResults;
    std::vector<std::uint8_t> doorCommands;

    // Engine: one J1939 ECU.
    Peer engine(coordinated("Engine", registry.uri, 10ms));
    auto& engineEcu = engine.j1939("J1939_CAN").add_node(
        {0x00, j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});
    engine.every(50ms, [&](SimTime) {
        const std::uint8_t data[2]{++engineCounter, 0};
        static_cast<void>(engineEcu.send(kPgnEngineSpeed, data));
    });

    // Body: a J1939 node and the LIN master; forwards the engine's value on LIN.
    Peer body(coordinated("Body", registry.uri, 5ms));
    auto& bodyEcu = body.j1939("J1939_CAN").add_node({0x21});
    bodyEcu.on_message(kPgnEngineSpeed, [&](const j1939::Message& m) { lastSpeed = m.data[0]; });
    auto& master = body.lin_master("BODY_LIN",
                                   {{kCommandId, 2, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Master},
                                    {kDoorId, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Slave}},
                                   [&](const lin::LinScheduleResult& r) { bodyResults.push_back(r); });
    body.on_step([&](SimTime now) {
        // The step's time in byte 1: the header sent in this step carries it (the application
        // runs before the LIN step).
        const std::uint8_t data[2]{lastSpeed, static_cast<std::uint8_t>(ms(now) / 5)};
        master.set_frame_data(kCommandId, data);
    });
    vehicle_sil::testkit::J1939Events bodyEvents(body.network("J1939_CAN"));

    // Door: a LIN slave.
    Peer door(coordinated("Door", registry.uri, 5ms));
    lin::LinSlaveOptions slaveOptions;
    slaveOptions.subscriptions = {kCommandId};
    auto& slave = door.lin_slave("BODY_LIN", slaveOptions, [&](const lin::LinReceivedFrame& f) { doorCommands.push_back(f.data[0]); });
    door.every(10ms, [&](SimTime now) {
        const std::uint8_t state[1]{static_cast<std::uint8_t>(ms(now) >= 500 ? 1 : 0)};
        slave.set_response(now, kDoorId, state);
    });

    start_together(controller, {&engine, &body, &door});
    for (Peer* p : {&engine, &body, &door})
    {
        const auto state = p->wait(60s);
        CHECK(state.has_value() && *state == orch::ParticipantState::Shutdown);
        CHECK(!p->failed());
    }

    CHECK(engineEcu.address_state() == j1939::AddressState::Claimed);
    // The body received the engine's broadcasts (sent at 0, 50, ..., 950 ms; the claim first).
    const auto speeds = bodyEvents.of(EventKind::MessageReceived, [](const auto& r) { return r.event.pgn == kPgnEngineSpeed; });
    CHECK(speeds.size() >= 19u);
    CHECK_EQ(speeds.front().data[0], 1u);
    CHECK(speeds.front().event.time == at(0ms));
    // LIN: the door answered, and saw the engine's value forwarded by the body.
    std::size_t doorOk = 0;
    bool doorOpenSeen = false;
    for (const auto& r : bodyResults)
    {
        if (r.id == kDoorId && r.status == lin::LinResultStatus::Ok)
        {
            ++doorOk;
            doorOpenSeen = doorOpenSeen || r.data[0] == 1;
        }
    }
    CHECK(doorOk >= 20u);
    CHECK(doorOpenSeen);
    std::size_t commands = 0;
    for (const auto& r : bodyResults)
    {
        if (r.id == kCommandId)
        {
            ++commands;
            CHECK_EQ(static_cast<int>(r.data[1]), static_cast<int>((ms(r.header_time) / 5) & 0xFF));
        }
    }
    CHECK(commands >= 20u); // every 40 ms
    CHECK(!doorCommands.empty());
    CHECK(doorCommands.back() >= 15u);
}

namespace {

/// Brake and engine on J1939: the engine claims, broadcasts and answers a request with a
/// long message (RTS/CTS); the brake records what it receives. On the local virtual bus or on
/// SIL Kit through Peer, with the same steps.
struct BrakeModel
{
    Seen seen;
};

void install_engine_ecu(j1939::Network& can)
{
    auto& ecu = can.add_node({0x00, j1939::Name{}.set_identity_number(0x2345).set_manufacturer_code(0x7F0)});
    ecu.on_request(kPgnSoftwareId, [](j1939::RequestContext& r) {
        std::vector<std::uint8_t> id(40, 0x41);
        static_cast<void>(r.respond(id));
    });
}

void engine_step(j1939::Network& can, int& sent, SimTime now)
{
    if (ms(now) % 20 == 0)
    {
        const std::uint8_t data[2]{static_cast<std::uint8_t>(++sent), 0};
        static_cast<void>(can.node(0).send(kPgnEngineSpeed, data));
    }
}

void install_brake_ecu(j1939::Network& can, BrakeModel& model)
{
    auto& ecu = can.add_node({0x0B});
    ecu.on_any_message([&model](const j1939::Message& m) { model.seen.add(m); });
}

void brake_step(j1939::Network& can, SimTime now)
{
    if (now == at(200ms))
    {
        static_cast<void>(can.node(0).request(kPgnSoftwareId, 0x00));
    }
}

std::vector<std::string> on_local_bus()
{
    BrakeModel model;
    int sent = 0;
    vehicle_sil::testkit::LocalCan bus;
    auto& engineCan = bus.add_j1939("engine", 10ms);
    install_engine_ecu(engineCan);
    bus.on_step("engine", [&](SimTime now) { engine_step(engineCan, sent, now); });
    auto& brakeCan = bus.add_j1939("brake", 5ms);
    install_brake_ecu(brakeCan, model);
    bus.on_step("brake", [&](SimTime now) { brake_step(brakeCan, now); });
    bus.run_until(at(600ms));
    return model.seen.lines;
}

std::vector<std::string> on_silkit()
{
    BrakeModel model;
    int sent = 0;
    InProcessRegistry registry;
    SimulationControllerOptions control;
    control.registry_uri = registry.uri;
    control.required_participants = {"Engine", "Brake"};
    control.stop_at = 600ms;
    SimulationController controller(control);
    Peer engine(coordinated("Engine", registry.uri, 10ms));
    auto& engineCan = engine.j1939("J1939_CAN");
    install_engine_ecu(engineCan);
    engine.on_step([&](SimTime now) { engine_step(engineCan, sent, now); });
    Peer brake(coordinated("Brake", registry.uri, 5ms));
    auto& brakeCan = brake.j1939("J1939_CAN");
    install_brake_ecu(brakeCan, model);
    brake.on_step([&](SimTime now) { brake_step(brakeCan, now); });
    start_together(controller, {&engine, &brake});
    CHECK(engine.wait(60s) == orch::ParticipantState::Shutdown);
    CHECK(brake.wait(60s) == orch::ParticipantState::Shutdown);
    CHECK(!engine.failed());
    CHECK(!brake.failed());
    return model.seen.lines;
}

} // namespace

TEST_CASE(peer_gives_the_same_results_as_the_local_virtual_bus)
{
    const auto local = on_local_bus();
    const auto silkit = on_silkit();
    CHECK(local.size() > 25u);
    bool longMessage = false;
    for (const auto& line : local)
    {
        longMessage = longMessage || line.rfind(std::to_string(kPgnSoftwareId.value()) + " ", 0) == 0;
    }
    CHECK(longMessage);
    CHECK(silkit == local);
}

TEST_CASE(peer_runs_alone_until_the_end_of_its_virtual_time)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Alone";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(200ms)};
    std::vector<std::int64_t> steps;
    std::vector<std::int64_t> periodic;
    Peer peer(o);
    peer.on_step([&](SimTime now) { steps.push_back(ms(now)); });
    peer.every(30ms, [&](SimTime now) { periodic.push_back(ms(now)); });
    peer.every(25ms, [&](SimTime now) { periodic.push_back(1000 + ms(now)); }); // not a multiple of the step
    CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
    CHECK(!steps.empty());
    CHECK_EQ(steps.front(), 0);
    CHECK(steps.back() <= 200);
    std::vector<std::int64_t> expected;
    for (const std::int64_t t : steps)
    {
        if (t % 30 == 0)
        {
            expected.push_back(t);
        }
        // 25 ms: the first step at or after 0, 25, 50, ...: 0, 30, 50, 80, 100, 130, 150, ...
        if (t % 50 == 0 || t % 50 == 30)
        {
            expected.push_back(1000 + t);
        }
    }
    CHECK(periodic == expected);
}

TEST_CASE(peer_stops_from_a_step_and_reports_failures)
{
    InProcessRegistry registry;
    {
        PeerOptions o;
        o.participant = "Stopper";
        o.registry_uri = registry.uri;
        o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, std::nullopt};
        Peer peer(o);
        std::int64_t last = 0;
        peer.on_step([&](SimTime now) {
            last = ms(now);
            if (now == at(100ms))
            {
                peer.request_stop("done");
            }
        });
        CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
        CHECK(last >= 100);
        CHECK(!peer.failed());
    }
    {
        PeerOptions o;
        o.participant = "Failing";
        o.registry_uri = registry.uri;
        o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, std::nullopt};
        Peer peer(o);
        peer.on_step([&](SimTime now) {
            if (now == at(50ms))
            {
                throw std::runtime_error("model failed");
            }
        });
        peer.start();
        const auto deadline = std::chrono::steady_clock::now() + 30s;
        while (!peer.failed() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(5ms);
        }
        CHECK(peer.failed());
        CHECK(peer.error().find("model failed") != std::string::npos);
        peer.stop();
    }
}

TEST_CASE(peer_setup_errors)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Setup";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(50ms)};
    Peer peer(o);
    peer.j1939("CAN1");
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("CAN1"); }));
    CHECK(throws<std::invalid_argument>([&] { peer.j1939(""); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(peer.network("CAN2")); }));
    CHECK(throws<std::invalid_argument>([&] { peer.every(0ms, [](SimTime) {}); }));
    CHECK(throws<std::invalid_argument>([&] { peer.on_step({}); }));
    // A real-time port on a peer with virtual time: the host refuses it, nothing is kept.
    class RealTimeOnly final : public can::Port
    {
    public:
        [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::RealTime; }
        std::size_t release_before(SimTime, can::FrameSink) override { return 0; }
        can::SendResult send(const can::TimedFrame&) override { return can::SendResult::Accepted; }
    };
    CHECK(throws<std::logic_error>([&] { peer.j1939("XL", std::make_unique<RealTimeOnly>()); }));
    vehicle_sil::J1939ChannelOptions bad;
    bad.port.transmit_queue_capacity = bad.port.event_capacity + 1; // refused by the SIL Kit port
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("BAD", bad); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(peer.network("BAD")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(peer.network("XL")); }));
    CHECK(peer.silkit_port("CAN1") != nullptr);
    CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
    CHECK(throws<std::logic_error>([&] { peer.j1939("CAN3"); }));
    CHECK(throws<std::logic_error>([&] { peer.on_step([](SimTime) {}); }));
    CHECK(throws<std::logic_error>([&] { peer.start(); }));

    PeerOptions zeroEnd = o;
    zeroEnd.participant = "ZeroEnd";
    zeroEnd.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(0ms)};
    CHECK(throws<std::invalid_argument>([&] { Peer never(zeroEnd); }));

    PeerOptions rt;
    rt.participant = "RealTimeSetup";
    rt.registry_uri = registry.uri;
    rt.virtual_time = std::nullopt;
    Peer realTime(rt);
    CHECK(realTime.real_time_clock() != nullptr);
    CHECK(throws<std::logic_error>([&] { static_cast<void>(realTime.lin_slave("LIN1")); }));
    CHECK(throws<std::logic_error>([&] { static_cast<void>(realTime.lin_master("LIN1", {})); }));
}

namespace {

/// A port of TimeDomain::RealTime fed by the test (stands for a Vector XL adapter).
class FakeRealTimePort final : public can::Port
{
public:
    [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::RealTime; }
    std::size_t release_before(SimTime now, can::FrameSink sink) override
    {
        std::deque<can::TimedFrame> ready;
        {
            std::lock_guard lock(mutex_);
            while (!inbox_.empty() && inbox_.front().time < now)
            {
                ready.push_back(inbox_.front());
                inbox_.pop_front();
            }
        }
        for (const auto& f : ready)
        {
            sink(f);
        }
        return ready.size();
    }
    can::SendResult send(const can::TimedFrame& frame) override
    {
        std::lock_guard lock(mutex_);
        sent_.push_back(frame);
        cv_.notify_all();
        return can::SendResult::Accepted;
    }
    void set_wakeup(can::Wakeup wakeup) override
    {
        std::lock_guard lock(mutex_);
        wakeup_ = std::move(wakeup);
    }
    void push(const can::TimedFrame& frame)
    {
        std::lock_guard lock(mutex_);
        inbox_.push_back(frame);
        if (wakeup_)
        {
            wakeup_(frame.time);
        }
    }
    /// Waits until a frame with this PGN was sent.
    bool wait_sent(j1939::Pgn pgn, std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [&] {
            for (const auto& f : sent_)
            {
                const auto id = j1939::Identifier::decode(f.frame);
                if (id && id->pgn == pgn)
                {
                    return true;
                }
            }
            return false;
        });
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<can::TimedFrame> inbox_;
    std::vector<can::TimedFrame> sent_;
    can::Wakeup wakeup_;
};

} // namespace

TEST_CASE(peer_runs_in_real_time_with_an_external_port)
{
    InProcessRegistry registry;
    can::ManualRealTimeClock clock;
    PeerOptions o;
    o.participant = "RealTime";
    o.registry_uri = registry.uri;
    o.virtual_time = std::nullopt;
    o.real_time_clock = &clock;
    o.max_idle = 5ms;
    Peer peer(o);
    CHECK(peer.real_time_clock() == &clock);
    auto port = std::make_unique<FakeRealTimePort>();
    FakeRealTimePort& fake = *port;
    auto& ecu = peer.j1939("XL1", std::move(port)).add_node({0x30});
    ecu.on_request(kPgnSoftwareId, [](j1939::RequestContext& r) {
        const std::uint8_t id[4]{'V', '1', '*', 0xFF};
        static_cast<void>(r.respond(id));
    });
    // A SIL Kit CAN port in real-time mode next to it.
    peer.j1939("J1939_CAN").add_node({0x31});
    CHECK(peer.silkit_port("XL1") == nullptr);
    CHECK(peer.silkit_port("J1939_CAN")->time_domain() == can::TimeDomain::RealTime);
    std::atomic<int> steps{0};
    peer.on_step([&](SimTime) { ++steps; });
    peer.start();
    can::Frame request;
    request.id = j1939::Identifier{6, j1939::Pgn{0xEA00}, 0x30, 0x44}.encode();
    request.format = can::IdFormat::Extended;
    request.length = 3;
    request.data[0] = 0xDA;
    request.data[1] = 0xFE;
    request.data[2] = 0x00;
    fake.push(can::TimedFrame{clock.now(), request});
    CHECK(fake.wait_sent(kPgnSoftwareId, 20s));
    CHECK(steps.load() > 0);
    peer.stop();
    CHECK(!peer.failed());
}

TEST_CASE(peer_stop_from_two_threads)
{
    InProcessRegistry registry;
    for (const bool realTime : {false, true})
    {
        PeerOptions o;
        o.participant = realTime ? "StopRealTime" : "StopVirtual";
        o.registry_uri = registry.uri;
        if (realTime)
        {
            o.virtual_time = std::nullopt;
        }
        else
        {
            o.virtual_time = VirtualTimeOptions{1ms, 1.0, std::nullopt}; // paced: still running when stopped
        }
        auto peer = std::make_unique<Peer>(o);
        auto& ecu = peer->j1939("J1939_CAN").add_node({0x30});
        std::atomic<int> steps{0};
        Peer* self = peer.get();
        peer->on_step([&steps, &ecu, self](SimTime) {
            ++steps;
            // Allowed in a step, also while another thread is in stop().
            if (self->failed() || !self->error().empty())
            {
                throw std::logic_error("unexpected failure");
            }
            const std::uint8_t data[1]{1};
            static_cast<void>(ecu.send(kPgnEngineSpeed, data));
        });
        peer->start();
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (steps.load() < 5 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        CHECK(steps.load() >= 5);
        std::thread other([&] { peer->stop("other thread"); });
        peer->stop("main thread");
        other.join();
        const int after = steps.load();
        std::this_thread::sleep_for(20ms);
        CHECK_EQ(steps.load(), after); // no step after stop()
        CHECK(!peer->failed());
        CHECK(peer->error().empty());
        peer.reset();
    }
}

TEST_CASE(peer_wait_returns_when_another_thread_stops)
{
    // wait() on one thread and stop() on another both finish the lifecycle (no data race).
    InProcessRegistry registry;
    for (const bool realTime : {false, true})
    {
        PeerOptions o;
        o.participant = realTime ? "WaitRealTime" : "WaitVirtual";
        o.registry_uri = registry.uri;
        if (realTime)
        {
            o.virtual_time = std::nullopt;
        }
        else
        {
            o.virtual_time = VirtualTimeOptions{1ms, 1.0, std::nullopt}; // paced: still running when stopped
        }
        Peer peer(o);
        static_cast<void>(peer.j1939("J1939_CAN").add_node({0x30}));
        std::atomic<int> steps{0};
        peer.on_step([&steps](SimTime) { ++steps; });
        peer.start();
        std::optional<orch::ParticipantState> waited;
        std::thread waiter([&] { waited = peer.wait(30s); });
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (steps.load() < 5 && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
        peer.stop("main thread");
        waiter.join();
        CHECK(waited == std::optional<orch::ParticipantState>{orch::ParticipantState::Shutdown});
        CHECK(peer.wait(1s) == std::optional<orch::ParticipantState>{orch::ParticipantState::Shutdown});
        CHECK(!peer.failed());
    }
}

TEST_CASE(peer_start_together_reports_a_failed_start)
{
    InProcessRegistry registry;
    SimulationControllerOptions control;
    control.registry_uri = registry.uri;
    control.required_participants = {"Member"};
    control.stop_at = 1s;
    SimulationController controller(control);
    PeerOptions alone;
    alone.participant = "StartedBefore";
    alone.registry_uri = registry.uri;
    alone.virtual_time = std::nullopt; // not in the time synchronisation of the others
    Peer started(alone);
    started.start(); // a second start() throws std::logic_error
    Peer member(coordinated("Member", registry.uri, 10ms));
    CHECK(throws<std::logic_error>([&] { start_together(controller, {&member, &started}); }));
    CHECK(member.wait(60s) == orch::ParticipantState::Shutdown);
    started.stop();
}

namespace {

/// What the cab of examples/j1939/ecu_pair shows, at each change before 1 s.
struct DashboardTrace
{
    std::vector<std::string> lines;
    void record(SimTime now, const ecu_pair::Dashboard& dash)
    {
        const std::string line = std::to_string(dash.engine_rpm) + (dash.engine_silent ? " silent " : " ") +
                                 std::to_string(dash.engine_software.size());
        if (now < at(1000ms) && (lines.empty() || lines.back().substr(lines.back().find(' ') + 1) != line))
        {
            lines.push_back(std::to_string(ms(now)) + " " + line);
        }
    }
};

std::vector<std::string> ecu_pair_on_local_bus()
{
    ecu_pair::EngineModel engine;
    ecu_pair::Dashboard dash;
    DashboardTrace trace;
    vehicle_sil::testkit::LocalCan bus;
    ecu_pair::install_engine(bus.add_ecu("engine", 10ms), engine);
    ecu_pair::install_cab(bus.add_ecu("cab", 5ms), dash);
    bus.on_step("cab", [&](SimTime now) { trace.record(now, dash); });
    bus.run_until(at(1000ms));
    return trace.lines;
}

std::vector<std::string> ecu_pair_on_silkit()
{
    ecu_pair::EngineModel engine; // what the steps use: declared before the peers
    ecu_pair::Dashboard dash;
    DashboardTrace trace;
    InProcessRegistry registry;
    SimulationControllerOptions control;
    control.registry_uri = registry.uri;
    control.required_participants = {"Engine", "Cab"};
    control.stop_at = 1100ms;
    SimulationController controller(control);
    Peer enginePeer(coordinated("Engine", registry.uri, 10ms));
    ecu_pair::install_engine(enginePeer, engine); // a Peer is an Ecu
    Peer cabPeer(coordinated("Cab", registry.uri, 5ms));
    ecu_pair::install_cab(cabPeer, dash);
    cabPeer.on_step([&](SimTime now) { trace.record(now, dash); });
    start_together(controller, {&enginePeer, &cabPeer});
    CHECK(enginePeer.wait(60s) == orch::ParticipantState::Shutdown);
    CHECK(cabPeer.wait(60s) == orch::ParticipantState::Shutdown);
    CHECK(!enginePeer.failed() && !cabPeer.failed());
    return trace.lines;
}

} // namespace

// P3: the ECU application's install functions on two SIL Kit participants, unchanged, give what
// they give on the local virtual bus.
TEST_CASE(peer_runs_the_ecu_application_as_on_the_local_bus)
{
    const auto local = ecu_pair_on_local_bus();
    const auto silkit = ecu_pair_on_silkit();
    CHECK(local.size() > 10u);
    // The engine's speed reached the cab's request, and the software id (14 bytes) arrived.
    CHECK(!local.empty() && local.back().substr(local.back().find(' ') + 1) == "1500 14");
    CHECK(silkit == local);
}

namespace {

/// Writes its name into `log` when destroyed.
struct Probe
{
    std::vector<std::string>& log;
    std::string name;
    Probe(std::vector<std::string>& l, std::string n) : log(l), name(std::move(n)) {}
    Probe(const Probe&) = delete;
    Probe& operator=(const Probe&) = delete;
    ~Probe() { log.push_back(name); }
};

/// An application written against Ecu& only: a counter it owns, sent every 20 ms.
struct Counter
{
    std::uint8_t value = 0;
    std::vector<std::int64_t> sent;
    explicit Counter(vehicle_sil::Ecu& ecu)
    {
        auto& node = ecu.j1939("CAN").add_node({.address = 0x10});
        ecu.every(20ms, [this, &node](SimTime now) {
            const std::uint8_t data[8]{value++, 0, 0, 0, 0, 0, 0, 0};
            static_cast<void>(node.send(kPgnEngineSpeed, data));
            sent.push_back(ms(now));
        });
    }
};

} // namespace

// Peer is an Ecu: an application written against Ecu& runs on it; what it emplace()s goes after
// everything else of the peer, the object emplaced last first; detail::set_virtual_time_end()
// ends its steps; emplace() after start() makes no object.
TEST_CASE(peer_is_an_ecu_and_destroys_what_it_emplaced_last)
{
    InProcessRegistry registry;
    std::vector<std::string> log;
    const Counter* counter = nullptr;
    {
        PeerOptions o;
        o.participant = "Alone";
        o.registry_uri = registry.uri;
        o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(200ms)};
        Peer peer(o);
        vehicle_sil::Ecu& ecu = peer;
        ecu.emplace<Probe>(log, "model");
        counter = &ecu.emplace<Counter>(ecu);
        auto handler = std::make_shared<Probe>(log, "handler");
        peer.network("CAN").node(0).on_message(kPgnEngineSpeed, [handler](const j1939::Message&) {});
        handler.reset();
        ecu.emplace<Probe>(log, "app");
        CHECK(throws<std::invalid_argument>([&] { vehicle_sil::detail::set_virtual_time_end(peer, SimTime{}); }));
        vehicle_sil::detail::set_virtual_time_end(peer, at(95ms));  // earlier than virtual_time.end: it applies
        vehicle_sil::detail::set_virtual_time_end(peer, at(150ms)); // later: the earlier one stays
        CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
        CHECK((counter->sent == std::vector<std::int64_t>{0, 20, 40, 60, 80}));
        CHECK(throws<std::logic_error>([&] { ecu.emplace<Probe>(log, "late"); }));
        CHECK(throws<std::logic_error>([&] { vehicle_sil::detail::set_virtual_time_end(peer, at(50ms)); }));
        CHECK(log.empty()); // no "late" was made
    }
    CHECK((log == std::vector<std::string>{"handler", "app", "model"}));

    // detail::set_virtual_time_end() needs virtual time.
    PeerOptions realTime;
    realTime.participant = "RealTime";
    realTime.registry_uri = registry.uri;
    realTime.virtual_time.reset();
    Peer peer(realTime);
    CHECK(throws<std::logic_error>([&] { vehicle_sil::detail::set_virtual_time_end(peer, at(50ms)); }));
}

// The SIL Kit CAN controllers of one peer have different names: a raw channel's "<network>.raw"
// and a J1939 network's (its name, or J1939ChannelOptions::controller_name) are checked against
// each other before SIL Kit is asked. Empty network names are refused through Ecu& too.
TEST_CASE(peer_refuses_a_controller_name_it_uses)
{
    InProcessRegistry registry;
    PeerOptions o;
    o.participant = "Names";
    o.registry_uri = registry.uri;
    o.virtual_time = VirtualTimeOptions{10ms, std::nullopt, at(30ms)};
    Peer peer(o);
    vehicle_sil::Ecu& ecu = peer;
    ecu.can("A");                                                         // controller "A.raw"
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("A.raw"); }));   // its name
    vehicle_sil::J1939ChannelOptions named;
    named.controller_name = "A.raw";
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("B", named); })); // its controller_name
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(peer.network("A.raw")); }));
    CHECK(throws<std::out_of_range>([&] { static_cast<void>(peer.network("B")); }));
    named.controller_name = "C.raw";
    peer.j1939("B", named);
    CHECK(throws<std::invalid_argument>([&] { ecu.can("C"); }));   // "C.raw" is B's
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("C.raw"); }));
    named.controller_name = "B2";
    peer.j1939("D", named);
    CHECK(throws<std::invalid_argument>([&] { peer.j1939("B2"); })); // between J1939 networks too
    ecu.j1939("A");  // the controller "A" beside "A.raw": allowed
    ecu.can("D");    // "D.raw"
    CHECK(throws<std::invalid_argument>([&] { ecu.j1939(""); }));
    CHECK(throws<std::invalid_argument>([&] { ecu.can(""); }));
    CHECK((peer.network_names() == std::vector<std::string>{"B", "D", "A"}));
    CHECK(peer.run(60s) == orch::ParticipantState::Shutdown);
    CHECK(!peer.failed());
}
