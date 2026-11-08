// SPDX-License-Identifier: MIT
//
// The J1939 stack does not allocate memory in steady-state steps: receiving, handling,
// answering requests, sending, local loopback and timers. This executable replaces the global
// operator new to count the calls (with ThreadSanitizer, whose runtime defines operator new,
// it counts through the sanitizer's allocator hooks instead), so it is built on its own.
#include "j1939_bench.hpp"
#include "network_test_access.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/j1939/broadcast_suppression.hpp"
#include "vehicle_sil/j1939/diagnostics.hpp"
#include "vehicle_sil/j1939/identification.hpp"
#include "vehicle_sil/j1939/network.hpp"
#include "vehicle_sil/j1939/observer.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>
#include <optional>
#include <string>
#include <vector>

#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define VEHICLE_SIL_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define VEHICLE_SIL_TSAN 1
#endif

using vehicle_sil::j1939::detail::NetworkTestAccess;

namespace {

std::atomic<bool> g_counting{false};
std::atomic<std::size_t> g_allocations{0};

void count_allocation()
{
    if (g_counting.load(std::memory_order_relaxed))
    {
        g_allocations.fetch_add(1, std::memory_order_relaxed);
    }
}

} // namespace

#if defined(VEHICLE_SIL_TSAN)
// The ThreadSanitizer runtime defines operator new itself; count through its allocator hooks.
#include <sanitizer/allocator_interface.h>

namespace {

void malloc_hook(const volatile void*, std::size_t) { count_allocation(); }
void free_hook(const volatile void*) {}
[[maybe_unused]] const int g_hooks_installed = __sanitizer_install_malloc_and_free_hooks(malloc_hook, free_hook);

} // namespace
#else
namespace {

void* allocate(std::size_t size)
{
    count_allocation();
    if (void* p = std::malloc(size == 0 ? 1 : size))
    {
        return p;
    }
    throw std::bad_alloc();
}

} // namespace

void* operator new(std::size_t size) { return allocate(size); }
void* operator new[](std::size_t size) { return allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try
    {
        return allocate(size);
    }
    catch (...)
    {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept { return operator new(size, tag); }
void* operator new(std::size_t size, std::align_val_t align)
{
    count_allocation();
    const auto a = static_cast<std::size_t>(align);
    if (void* p = std::aligned_alloc(a, ((size == 0 ? 1 : size) + a - 1) / a * a))
    {
        return p;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t align) { return operator new(size, align); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
#endif

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::SimTime;
using vehicle_sil::j1939::AckCode;
using vehicle_sil::j1939::Message;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::RequestContext;
using vehicle_sil::j1939::SendStatus;
using vehicle_sil::j1939::TimerKind;
using vehicle_sil::j1939::TimerTarget;

namespace {

/// Restarts itself every 7 ms and sends a frame.
struct PeriodicTimer final : TimerTarget
{
    vehicle_sil::j1939::Network* network = nullptr;
    Node* node = nullptr;
    SimTime next{};
    void on_timer(std::uint32_t tag) override
    {
        const std::array<std::uint8_t, 2> data{static_cast<std::uint8_t>(tag), 0x55};
        CHECK(node->send(Pgn{0xFF20}, data) == SendStatus::Ok);
        next += std::chrono::milliseconds(7);
        NetworkTestAccess::timers(*network).start(TimerKind::Scheduled, next, *this, tag + 1);
    }
};

} // namespace

TEST_CASE(steady_state_steps_do_not_allocate)
{
    Bench bench;
    Node& a = bench.add_node(0x80);
    Node& b = bench.add_node(0x81);
    std::size_t messages = 0;
    std::size_t bytes = 0;
    a.on_any_message([&](const Message& m) {
        ++messages;
        bytes += m.data.size();
    });
    b.on_message(Pgn{kPgnPropB}, [&](const Message& m) {
        // Answer every broadcast from the tester with a frame to it.
        if (m.source == 0x90)
        {
            const std::array<std::uint8_t, 1> one{m.data.empty() ? std::uint8_t{0} : m.data[0]};
            CHECK(b.send(Pgn{kPgnPropA}, one, 0x90, 3) == SendStatus::Ok);
        }
    });
    a.on_request(Pgn{kPgnPropB}, [](RequestContext& c) {
        const std::array<std::uint8_t, 8> data{1, 2, 3, 4, 5, 6, 7, 8};
        CHECK(c.respond(data) == SendStatus::Ok);
    });
    b.on_any_request([](RequestContext& c) { c.acknowledge(AckCode::CannotRespond); });
    bench.app = [&](SimTime now) {
        const std::array<std::uint8_t, 3> data{static_cast<std::uint8_t>(vehicle_sil::to_ns(now) / 1000000), 1, 2};
        CHECK(a.send(Pgn{kPgnPropB}, data) == SendStatus::Ok);
        CHECK(a.request(Pgn{0xFEF1}, 0x81) == SendStatus::Ok);
    };
    PeriodicTimer timer;
    timer.network = &bench.network;
    timer.node = &b;
    timer.next = at(std::chrono::milliseconds(5));
    NetworkTestAccess::timers(bench.network).start(TimerKind::Scheduled, timer.next, timer, 1);

    const auto inject_traffic = [&] {
        const SimTime t = bench.next_time();
        bench.inject(t, frame(6, kPgnPropB, 0x90, kGlobal, {9, 8, 7}));
        bench.inject(t, frame(6, kPgnRequest, 0x90, 0x80, request(kPgnPropB)));
        bench.inject(t, frame(6, kPgnRequest, 0x90, 0x81, request(0xFEF1)));
        bench.inject(t, frame(6, kPgnRequest, 0x90, 0x80, request(0xFEF2)));
        bench.inject(t, frame(3, kPgnPropA, 0x91, 0x80, {1}));
        bench.inject(t, frame(3, kPgnPropA, 0x92, 0x55, {1})); // to nobody here
    };

    // Warm up: the first steps may grow what the test itself uses.
    for (int i = 0; i < 5; ++i)
    {
        inject_traffic();
        bench.step();
        static_cast<void>(bench.take_sent());
    }
    std::size_t counted = 0;
    for (int i = 0; i < 50; ++i)
    {
        inject_traffic();
        g_allocations.store(0);
        g_counting.store(true);
        bench.step();
        g_counting.store(false);
        counted += g_allocations.load();
        static_cast<void>(bench.take_sent());
    }
    CHECK_EQ(counted, 0u);
    CHECK(messages > 200u);
    CHECK(bytes > 0u);
    CHECK_EQ(bench.network.statistics().receive_overflows, 0u);
    CHECK_EQ(bench.network.statistics().control_frames_dropped, 0u);
}

TEST_CASE(the_counter_sees_allocations)
{
    g_allocations.store(0);
    g_counting.store(true);
    void* p = ::operator new(16); // a direct call: a new-expression may be optimised away
    g_counting.store(false);
    ::operator delete(p);
    CHECK_EQ(g_allocations.load(), 1u);
}

namespace {

/// Forwards to a bus port but answers QueueFull to every 5th and Failed to every 7th send.
class FlakyPort final : public vehicle_sil::can::Port
{
public:
    explicit FlakyPort(vehicle_sil::can::Port& inner) : inner_(inner) {}
    [[nodiscard]] vehicle_sil::can::TimeDomain time_domain() const noexcept override { return inner_.time_domain(); }
    std::size_t release_before(SimTime now, vehicle_sil::can::FrameSink sink) override { return inner_.release_before(now, sink); }
    vehicle_sil::can::SendResult send(const vehicle_sil::can::TimedFrame& frame) override
    {
        ++calls_;
        if (calls_ % 5 == 0)
        {
            return vehicle_sil::can::SendResult::QueueFull;
        }
        if (calls_ % 7 == 0)
        {
            return vehicle_sil::can::SendResult::Failed;
        }
        return inner_.send(frame);
    }

private:
    vehicle_sil::can::Port& inner_;
    std::size_t calls_ = 0;
};

} // namespace

TEST_CASE(steady_state_with_a_full_port_failures_and_carry_over_does_not_allocate)
{
    vehicle_sil::can::VirtualCanBus bus;
    vehicle_sil::can::Port& tester = bus.attach();
    FlakyPort port(bus.attach());
    vehicle_sil::j1939::NetworkOptions options;
    options.max_frames_per_step = 3; // the application sends 4 per step: some always carry over
    options.data_queue_capacity = 64;
    vehicle_sil::j1939::Network network(options);
    Node& a = network.add_node({0x80});
    Node& b = network.add_node({0x81});
    std::size_t seen = 0;
    b.on_any_message([&](const Message&) { ++seen; });
    vehicle_sil::can::Host host;
    host.add(network, port);
    host.set_application([&](SimTime) {
        for (std::uint8_t i = 0; i < 4; ++i)
        {
            const std::array<std::uint8_t, 1> one{i};
            const SendStatus status = a.send(Pgn{kPgnPropB}, one);
            CHECK(status == SendStatus::Ok || status == SendStatus::QueueFull);
        }
    });
    SimTime now{};
    std::size_t counted = 0;
    for (int i = 0; i < 60; ++i)
    {
        tester.send(vehicle_sil::can::TimedFrame{now, to_can(frame(6, kPgnRequest, 0x90, 0x81, request(0xFEF1)))});
        const bool count = i >= 10;
        g_allocations.store(0);
        g_counting.store(count);
        host.step(now);
        g_counting.store(false);
        counted += count ? g_allocations.load() : 0;
        tester.release_before(now + std::chrono::nanoseconds(1), [](const vehicle_sil::can::TimedFrame&) {});
        now += std::chrono::milliseconds(10);
    }
    CHECK_EQ(counted, 0u);
    CHECK(seen > 100u);
    CHECK(network.statistics().send_queue_full > 0u);
    CHECK(network.statistics().send_failures > 0u);
}

namespace {

/// Counts the events of each kind (no allocation).
struct CountingObserver final : vehicle_sil::j1939::Observer
{
    void on_event(const vehicle_sil::j1939::Event& e) override { ++counts[static_cast<std::size_t>(e.kind)]; }
    std::array<std::size_t, 8> counts{};
};

} // namespace

TEST_CASE(address_claiming_does_not_allocate)
{
    // Claim, the wait with held messages and their release, contention won and lost, Cannot
    // Claim and Requests for Address Claimed, all inside the counted steps.
    Bench bench;
    vehicle_sil::j1939::NodeOptions winner;
    winner.address = 0x82;
    winner.name = vehicle_sil::j1939::Name{0x20008200FE001234u};
    vehicle_sil::j1939::NodeOptions loser;
    loser.address = 0x83;
    loser.name = vehicle_sil::j1939::Name{0x20008200FE005678u};
    Node& c = bench.network.add_node(winner);
    Node& d = bench.network.add_node(loser);
    CountingObserver observer; // also with an observer
    bench.network.set_observer(&observer);
    bench.app = [&](SimTime now) {
        const std::array<std::uint8_t, 2> data{1, 2};
        if (vehicle_sil::to_ns(now) % 30000000 == 0) // 9 messages during the wait: within hold_capacity
        {
            CHECK(c.send(Pgn{kPgnPropB}, data) == SendStatus::Ok);
        }
        static_cast<void>(d.send(Pgn{kPgnPropB}, data)); // held, then NoAddress after losing
    };
    bench.step(); // 0: warm-up step outside the count (first use of the bench)
    static_cast<void>(bench.take_sent());
    std::size_t counted = 0;
    for (int i = 1; i <= 60; ++i)
    {
        const SimTime t = bench.next_time() - std::chrono::milliseconds(5);
        if (i == 3)
        {
            bench.inject(t, frame(6, kPgnAddressClaim, 0x82, kGlobal, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
            bench.inject(t, frame(6, kPgnAddressClaim, 0x83, kGlobal, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}));
        }
        if (i % 5 == 0)
        {
            bench.inject(t, frame(6, kPgnRequest, 0x90, kGlobal, request(kPgnAddressClaim)));
        }
        g_allocations.store(0);
        g_counting.store(true);
        bench.step();
        g_counting.store(false);
        counted += g_allocations.load();
        static_cast<void>(bench.take_sent());
    }
    CHECK_EQ(counted, 0u);
    CHECK(c.address_state() == vehicle_sil::j1939::AddressState::Claimed);
    CHECK(d.address_state() == vehicle_sil::j1939::AddressState::CannotClaim);
    CHECK(bench.network.statistics().held_dropped > 0u);
    CHECK(observer.counts[static_cast<std::size_t>(vehicle_sil::j1939::EventKind::MessageDropped)] > 0u);
    CHECK(observer.counts[static_cast<std::size_t>(vehicle_sil::j1939::EventKind::AddressChanged)] > 0u);
}

TEST_CASE(address_management_does_not_allocate)
{
    // An arbitrary address capable node that loses and moves, the address table (claims, moves,
    // Cannot Claim), address violations, Commanded Address and the address change handler, all
    // inside the counted steps.
    Bench bench;
    vehicle_sil::j1939::NodeOptions movable;
    movable.address = 0x80;
    movable.name = vehicle_sil::j1939::Name{0xA0008200FE001234u};
    Node& m = bench.network.add_node(movable);
    std::size_t changes = 0;
    m.on_address_change([&](const vehicle_sil::j1939::AddressChange&) { ++changes; });
    const std::array<std::uint8_t, 9> command{0x34, 0x12, 0x00, 0xFE, 0x00, 0x82, 0x00, 0xA0, 0x99};
    bench.app = [&](SimTime now) {
        if (now == SimTime{} + std::chrono::milliseconds(400))
        {
            NetworkTestAccess::deliver_message(bench.network, 
                vehicle_sil::j1939::Message{Pgn{0xFED8}, 6, 0x90, kGlobal, now, std::span<const std::uint8_t>(command)});
        }
        static_cast<void>(m.send(Pgn{kPgnPropB}, {})); // held during the waits
    };
    bench.step(); // 0: warm-up step outside the count
    static_cast<void>(bench.take_sent());
    std::size_t counted = 0;
    const Bytes lower{0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    for (int i = 1; i <= 60; ++i)
    {
        const SimTime t = bench.next_time() - std::chrono::milliseconds(5);
        if (i == 2)
        {
            bench.inject(t, frame(6, kPgnAddressClaim, 0x81, kGlobal, {1, 0, 0, 0, 0, 0, 0, 0}));
            bench.inject(t, frame(6, kPgnAddressClaim, 0x80, kGlobal, lower)); // m moves to 0x82
        }
        if (i == 5)
        {
            bench.inject(t, frame(6, kPgnAddressClaim, 0x85, kGlobal, {1, 0, 0, 0, 0, 0, 0, 0})); // 0x81 moves
            bench.inject(t, frame(6, kPgnAddressClaim, kNull, kGlobal, lower));                   // 0x80 gives up
        }
        if (i % 7 == 0)
        {
            bench.inject(t, frame(6, kPgnPropB, m.address(), kGlobal, {})); // a violation
        }
        g_allocations.store(0);
        g_counting.store(true);
        bench.step();
        g_counting.store(false);
        counted += g_allocations.load();
        static_cast<void>(bench.take_sent());
    }
    CHECK_EQ(counted, 0u);
    CHECK_EQ(m.address(), 0x99);
    CHECK(changes >= 4u);
    CHECK(bench.network.statistics().address_violations > 0u);
    CHECK_EQ(bench.network.address_table().size(), 1u);
}

namespace {

// Two networks on one bus exchange broadcasts, connections and long responses, with lost,
// repeated and delayed packets (retransmission requests, timeouts and aborts). Network A has
// a second, arbitrary address capable node: transfers to its neighbour go through the local
// loopback, and it is made to move twice (sessions cancelled, transfers held during the wait
// after its claim). All inside the counted steps.
void check_transport_protocol_does_not_allocate(const vehicle_sil::can::VirtualCanBusOptions& options, bool observed = false)
{
    vehicle_sil::can::VirtualCanBus bus(options);
    auto& portA = bus.attach();
    auto& portB = bus.attach();
    auto& tester = bus.attach();
    vehicle_sil::j1939::Network a;
    vehicle_sil::j1939::Network b;
    vehicle_sil::can::Host hostA;
    vehicle_sil::can::Host hostB;
    hostA.add(a, portA);
    hostB.add(b, portB);
    CountingObserver observerA;
    CountingObserver observerB;
    if (observed)
    {
        a.set_observer(&observerA);
        b.set_observer(&observerB);
    }
    Node& na = a.add_node({0x10});
    constexpr std::uint64_t kMovable = 0xA000820000001234u;
    Node& movable = a.add_node({0x80, vehicle_sil::j1939::Name{kMovable}});
    Node& nb = b.add_node({0x20});
    std::array<std::uint8_t, 1785> big{};
    for (std::size_t i = 0; i < big.size(); ++i)
    {
        big[i] = static_cast<std::uint8_t>(i);
    }
    std::size_t received = 0;
    std::size_t ended = 0;
    na.on_any_message([&](const vehicle_sil::j1939::Message& m) { received += m.data.size() > 8 ? 1u : 0u; });
    nb.on_any_message([&](const vehicle_sil::j1939::Message& m) { received += m.data.size() > 8 ? 1u : 0u; });
    na.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd&) { ++ended; });
    nb.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd&) { ++ended; });
    nb.on_request(Pgn{0xFECA}, [&](vehicle_sil::j1939::RequestContext& c) {
        static_cast<void>(c.respond(std::span<const std::uint8_t>(big.data(), 300)));
    });
    std::uint32_t state = 7;
    portA.set_fault_injector([&state](const vehicle_sil::can::TimedFrame&) {
        state = state * 1664525u + 1013904223u;
        const auto r = (state >> 16) % 100u;
        if (r < 2u)
        {
            return vehicle_sil::can::FaultAction::drop();
        }
        if (r < 3u)
        {
            return vehicle_sil::can::FaultAction::duplicate();
        }
        return r < 4u ? vehicle_sil::can::FaultAction::delay_by(std::chrono::milliseconds(3))
                      : vehicle_sil::can::FaultAction::deliver();
    });
    movable.on_transfer_end([&](const vehicle_sil::j1939::TransferEnd&) { ++ended; });
    hostA.set_application([&](SimTime now) {
        const auto ms = (now - SimTime{}) / std::chrono::milliseconds(1);
        if (ms % 200 == 0)
        {
            static_cast<void>(na.send(Pgn{0xFECB}, std::span<const std::uint8_t>(big.data(), 100 + static_cast<std::size_t>(ms % 900))));
            static_cast<void>(na.send(Pgn{0xEF00}, std::span<const std::uint8_t>(big.data(), 1785), 0x20));
            static_cast<void>(na.request(Pgn{0xFECA}, 0x20));
        }
        if (ms % 150 == 0)
        {
            static_cast<void>(movable.send(Pgn{0xEF00}, std::span<const std::uint8_t>(big.data(), 200), 0x10)); // local
            static_cast<void>(movable.send(Pgn{0xEF00}, std::span<const std::uint8_t>(big.data(), 120), 0x20));
        }
    });
    hostB.set_application([&](SimTime now) {
        const auto ms = (now - SimTime{}) / std::chrono::milliseconds(1);
        if (ms % 300 == 0)
        {
            static_cast<void>(nb.send(Pgn{0xEF00}, std::span<const std::uint8_t>(big.data(), 50), 0x10));
            static_cast<void>(nb.send(Pgn{0xFECB}, std::span<const std::uint8_t>(big.data(), 9)));
        }
    });
    vehicle_sil::can::StepDriver driver;
    driver.add(hostA, std::chrono::milliseconds(10));
    driver.add(hostB, std::chrono::milliseconds(7));
    driver.run_until(SimTime{} + std::chrono::milliseconds(20)); // warm-up outside the count
    std::size_t counted = 0;
    std::size_t moves = 0;
    for (int i = 1; i <= 1000; ++i)
    {
        if (i == 300 || i == 600)
        {
            // A lower NAME claims the movable node's address: it moves (outside the count: the
            // tester builds the frame).
            vehicle_sil::can::Frame claim;
            claim.id = j1939_id(6, kPgnAddressClaim, kGlobal, movable.address());
            claim.format = vehicle_sil::can::IdFormat::Extended;
            claim.length = 8;
            const std::uint64_t lower = kMovable - 1 - static_cast<std::uint64_t>(i);
            for (int k = 0; k < 8; ++k)
            {
                claim.data[static_cast<std::size_t>(k)] = static_cast<std::uint8_t>(lower >> (8 * k));
            }
            tester.send(vehicle_sil::can::TimedFrame{SimTime{} + std::chrono::milliseconds(15 + 10 * i), claim});
            ++moves;
        }
        g_allocations.store(0);
        g_counting.store(true);
        driver.run_until(SimTime{} + std::chrono::milliseconds(20 + 10 * i));
        g_counting.store(false);
        counted += g_allocations.load();
    }
    CHECK_EQ(counted, 0u);
    CHECK(received > 20u);
    CHECK(ended > 20u);
    CHECK(a.statistics().transport_messages_sent > 5u);
    CHECK_EQ(moves, 2u);
    CHECK(movable.address() != 0x80);
    CHECK(a.statistics().transport_sessions_cancelled > 0u);
    tester.release_before(SimTime{} + std::chrono::milliseconds(20 + 10 * 1001), [](const vehicle_sil::can::TimedFrame&) {});
    CHECK(b.statistics().transport_retransmit_requests > 0u);
    if (observed)
    {
        for (std::size_t kind = 0; kind < 7; ++kind) // all but PortEvent
        {
            if (kind != static_cast<std::size_t>(vehicle_sil::j1939::EventKind::MessageDropped)) // see address_claiming_does_not_allocate
            {
                CHECK(observerA.counts[kind] + observerB.counts[kind] > 0u);
            }
        }
    }
}

} // namespace

TEST_CASE(transport_protocol_with_an_observer_does_not_allocate)
{
    check_transport_protocol_does_not_allocate({}, true);
}

TEST_CASE(transport_protocol_does_not_allocate)
{
    check_transport_protocol_does_not_allocate({});
}

TEST_CASE(transport_protocol_on_the_timed_bus_does_not_allocate)
{
    // The bus model, the transmit queues and the port events (counted by the networks) do not
    // allocate either, exact and with a simulator step.
    for (const auto step : {vehicle_sil::SimDuration{}, vehicle_sil::SimDuration{std::chrono::milliseconds(1)}})
    {
        vehicle_sil::can::VirtualCanBusOptions options;
        options.timing = vehicle_sil::can::VirtualCanBusTiming{500'000, 64, step};
        check_transport_protocol_does_not_allocate(options);
    }
}

// The toolbox and the protocol parts in steady state: Timers restarting, Subscriptions,
// periodic DM1 by BAM, DM13, requests for DM1/DM2/identification, DM22, deferred clears.
TEST_CASE(protocol_parts_and_the_toolbox_do_not_allocate_in_steady_state)
{
    using vehicle_sil::j1939::BroadcastSuppression;
    using vehicle_sil::j1939::ClearDecision;
    using vehicle_sil::j1939::Diagnostics;
    using vehicle_sil::j1939::DiagnosticsOptions;
    using vehicle_sil::j1939::Dtc;
    using vehicle_sil::j1939::DtcClear;
    using vehicle_sil::j1939::Identification;
    using vehicle_sil::j1939::Lamp;
    using vehicle_sil::j1939::Subscription;
    using vehicle_sil::j1939::Timer;
    Bench bench;
    Node& us = bench.add_node(0x20);
    DiagnosticsOptions options;
    options.on_clear = [](DtcClear kind) { return kind == DtcClear::Active ? ClearDecision::Defer : ClearDecision::Clear; };
    auto& diag = us.attach<Diagnostics>(options);
    auto& id = us.attach<Identification>();
    auto& suppression = BroadcastSuppression::of(us);
    const std::vector<std::string> software{"vehicle-sil 1.0", "allocation test"};
    id.set_software_identification(software);
    for (std::uint32_t spn = 1; spn <= 4; ++spn)
    {
        CHECK(diag.set_active(Dtc{spn, 2, Lamp::AmberWarning}, true));
    }
    std::size_t ticks = 0;
    std::optional<Timer> tick;
    tick.emplace(us, TimerKind::Scheduled, [&] {
        ++ticks;
        tick->start_after(std::chrono::milliseconds(7));
    });
    tick->start_at(at(std::chrono::milliseconds(3)));
    std::size_t seen = 0;
    Subscription sub{us, us.on_any_message([&](const Message&) { ++seen; })};

    int step = 0;
    const auto drive = [&] {
        const SimTime t = bench.next_time();
        ++step;
        bench.inject(t, frame(6, kPgnRequest, 0x90, kGlobal, request(0xFECA)));
        bench.inject(t, frame(6, kPgnRequest, 0x91, 0x20, request(0xFECB)));
        const std::uint8_t control = step % 2 ? 0x11 : 0x01;
        bench.inject(t, frame(6, 0xC300, 0x90, 0x20, {control, 0xFF, 0xFF, 0xFF, 0xFF, 0x05, 0x00, 0x02}));
        if (step % 50 == 0)
        {
            bench.inject(t, frame(6, kPgnRequest, 0x90, kGlobal, request(0xFEDA)));
            bench.inject(t, frame(6, kPgnRequest, 0x90, 0x20, request(0xFED3)));
            bench.inject(t, frame(6, 0xDF00, 0x90, kGlobal, {static_cast<std::uint8_t>(step % 100 ? 0xFC : 0xFD), 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
        }
        if (step % 50 == 25)
        {
            static_cast<void>(diag.complete_clear(DtcClear::Active, true));
            suppression.suspend(std::chrono::seconds(0));
        }
        static_cast<void>(diag.set_active(Dtc{5, 2}, step % 3 != 0));
    };
    for (int i = 0; i < 300; ++i)
    {
        drive();
        bench.step();
        static_cast<void>(bench.take_sent());
    }
    std::size_t counted = 0;
    for (int i = 0; i < 1500; ++i)
    {
        drive();
        g_allocations.store(0);
        g_counting.store(true);
        bench.step();
        g_counting.store(false);
        counted += g_allocations.load();
        static_cast<void>(bench.take_sent());
    }
    CHECK_EQ(counted, 0u);
    CHECK(ticks > 2000u);
    CHECK(seen > 1000u);
    CHECK(!bench.network.failed());
    CHECK(bench.network.statistics().transport_messages_sent > 10u);
}
