// SPDX-License-Identifier: MIT
//
// CAN bus timing (vehicle_sil::can): frame length, arbitration, CanBusModel, and the virtual bus
// with timing (exact and with a simulator step). Built without SIL Kit.
#include "test_harness.hpp"

#include "vehicle_sil/can/bus_model.hpp"
#include "vehicle_sil/can/host.hpp"
#include "vehicle_sil/can/step_driver.hpp"
#include "vehicle_sil/can/virtual_can_bus.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::SimDuration;
using vehicle_sil::SimTime;
using vehicle_sil::to_ns;
using vehicle_sil::can::arbitration_key;
using vehicle_sil::can::CanBusModel;
using vehicle_sil::can::CanBusModelOptions;
using vehicle_sil::can::FaultAction;
using vehicle_sil::can::Frame;
using vehicle_sil::can::frame_bits;
using vehicle_sil::can::frame_duration;
using vehicle_sil::can::goes_first;
using vehicle_sil::can::Host;
using vehicle_sil::can::IdFormat;
using vehicle_sil::can::Port;
using vehicle_sil::can::PortEvent;
using vehicle_sil::can::SendResult;
using vehicle_sil::can::Steppable;
using vehicle_sil::can::StepDriver;
using vehicle_sil::can::TimedFrame;
using vehicle_sil::can::VirtualCanBus;
using vehicle_sil::can::VirtualCanBusOptions;
using vehicle_sil::can::VirtualCanBusTiming;

namespace {

Frame ext(std::uint32_t id, std::uint8_t length = 8, std::uint8_t first = 0)
{
    Frame f;
    f.id = id;
    f.format = IdFormat::Extended;
    f.length = length;
    f.data[0] = first;
    return f;
}

Frame std11(std::uint32_t id, std::uint8_t length = 8, std::uint8_t first = 0)
{
    Frame f = ext(id, length, first);
    f.format = IdFormat::Standard;
    return f;
}

/// A J1939-style 29-bit identifier: priority, PGN, source address.
std::uint32_t j1939_id(std::uint32_t priority, std::uint32_t pgn, std::uint32_t source)
{
    return (priority << 26) | (pgn << 8) | source;
}

SimTime at(SimDuration t)
{
    return SimTime{} + t;
}

SimTime at_us(std::int64_t us)
{
    return SimTime{} + std::chrono::microseconds(us);
}

struct Tx
{
    std::size_t sender;
    std::uint32_t id;
    std::int64_t start_ns;
    std::int64_t end_ns;
    friend bool operator==(const Tx&, const Tx&) = default;
};

std::vector<Tx> run(CanBusModel& model, SimTime from, SimTime until)
{
    std::vector<Tx> out;
    model.run(from, until, [&](const CanBusModel::Transmission& tx) {
        out.push_back(Tx{tx.sender, tx.frame.id, to_ns(tx.start), to_ns(tx.end)});
    });
    return out;
}

constexpr std::int64_t kExt8At250k = 524'000; // 131 bits at 4 us
constexpr SimTime kForever = SimTime{} + 1000s;

} // namespace

TEST_CASE(frame_length_counts_the_bits_without_stuffing)
{
    CHECK_EQ(frame_bits(std11(0x100, 0)), 47u);
    CHECK_EQ(frame_bits(std11(0x100, 8)), 111u);
    CHECK_EQ(frame_bits(ext(0x100, 0)), 67u);
    CHECK_EQ(frame_bits(ext(0x100, 8)), 131u);
    CHECK_EQ(to_ns(SimTime{} + frame_duration(ext(0x100, 8), 250'000)), kExt8At250k);
    CHECK_EQ(to_ns(SimTime{} + frame_duration(std11(0x100, 0), 500'000)), 94'000);
    CHECK_EQ(to_ns(SimTime{} + frame_duration(ext(0x100, 1), 1'000'000)), 75'000);
    // Rounded up to whole nanoseconds: 47 bits at 33333 bit/s = 1410014.1 ns.
    CHECK_EQ(to_ns(SimTime{} + frame_duration(std11(0x100, 0), 33'333)), 1'410'015);
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { static_cast<void>(frame_duration(ext(1), 0)); }));
}

TEST_CASE(arbitration_compares_the_base_identifier_first_and_prefers_11_bit_frames)
{
    // Lower identifier wins, for both formats.
    CHECK(goes_first(std11(0x100), std11(0x101)));
    CHECK(goes_first(ext(0x1000), ext(0x1001)));
    CHECK(!goes_first(ext(0x1001), ext(0x1000)));
    // The same 11-bit base identifier: the 11-bit frame wins (RTR dominant against SRR).
    const std::uint32_t base = 0x123;
    CHECK(goes_first(std11(base), ext(base << 18)));
    CHECK(!goes_first(ext(base << 18), std11(base)));
    // A lower base identifier wins whatever the format.
    CHECK(goes_first(ext((base - 1) << 18 | 0x3FFFF), std11(base)));
    CHECK(goes_first(std11(base), ext((base + 1) << 18)));
    // J1939: priority 3 beats priority 6, whatever the PGN and source.
    CHECK(goes_first(ext(j1939_id(3, 0xFFFF, 0xFE)), ext(j1939_id(6, 0x0000, 0x00))));
    CHECK(arbitration_key(ext(base << 18)) > arbitration_key(std11(base)));
    // Equal identifiers: shorter payload, then payload bytes (deterministic, not a real-bus rule).
    CHECK(goes_first(ext(0x10, 2), ext(0x10, 3)));
    CHECK(goes_first(ext(0x10, 8, 1), ext(0x10, 8, 2)));
    CHECK(!goes_first(ext(0x10, 8, 2), ext(0x10, 8, 2)));
}

TEST_CASE(bus_model_transmits_simultaneous_frames_in_arbitration_order)
{
    CanBusModel model(CanBusModelOptions{250'000, 8});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    const auto c = model.add_sender();
    CHECK(model.submit(a, ext(j1939_id(6, 0xFF10, 0x80)), at(10ms)));
    CHECK(model.submit(b, ext(j1939_id(3, 0xFF30, 0x90)), at(10ms)));
    CHECK(model.submit(c, std11(0x7FF), at(10ms)));
    const auto txs = run(model, SimTime{}, kForever);
    const std::int64_t t0 = 10'000'000;
    const std::int64_t std8 = 444'000; // 111 bits at 4 us
    CHECK(txs == (std::vector<Tx>{{b, j1939_id(3, 0xFF30, 0x90), t0, t0 + kExt8At250k},
                                  {a, j1939_id(6, 0xFF10, 0x80), t0 + kExt8At250k, t0 + 2 * kExt8At250k},
                                  {c, 0x7FF, t0 + 2 * kExt8At250k, t0 + 2 * kExt8At250k + std8}}));
    CHECK_EQ(model.queued(), 0u);
    CHECK_EQ(model.transmissions(), 3u);
    CHECK_EQ(to_ns(model.bus_free()), t0 + 2 * kExt8At250k + std8);
}

TEST_CASE(bus_model_lets_frames_that_became_ready_during_a_transmission_compete_when_it_ends)
{
    CanBusModel model(CanBusModelOptions{250'000, 8});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    const auto c = model.add_sender();
    CHECK(model.submit(a, ext(0x500), at(0ms)));
    CHECK(model.submit(b, ext(0x300), at_us(100))); // during a's frame
    CHECK(model.submit(c, ext(0x200), at_us(400))); // also during a's frame: wins against b
    const auto txs = run(model, SimTime{}, kForever);
    CHECK(txs == (std::vector<Tx>{{a, 0x500, 0, kExt8At250k},
                                  {c, 0x200, kExt8At250k, 2 * kExt8At250k},
                                  {b, 0x300, 2 * kExt8At250k, 3 * kExt8At250k}}));
}

TEST_CASE(bus_model_keeps_each_senders_fifo_order)
{
    // A controller with one transmit FIFO: a's high-priority frame waits behind a's own
    // low-priority frame, and b's middle frame wins against that low-priority frame.
    CanBusModel model(CanBusModelOptions{500'000, 8});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    CHECK(model.submit(a, ext(0x900), at(0ms)));
    CHECK(model.submit(a, ext(0x100), at(0ms)));
    CHECK(model.submit(b, ext(0x500), at(0ms)));
    const auto txs = run(model, SimTime{}, kForever);
    CHECK_EQ(txs.size(), 3u);
    CHECK_EQ(txs[0].id, 0x500u);
    CHECK_EQ(txs[1].id, 0x900u);
    CHECK_EQ(txs[2].id, 0x100u);
    // A frame submitted with an earlier ready time than the one ahead of it waits for it.
    CHECK(model.submit(a, ext(0x900), at(10ms)));
    CHECK(model.submit(a, ext(0x100), at(5ms)));
    const auto later = run(model, SimTime{}, kForever);
    CHECK_EQ(later.size(), 2u);
    CHECK_EQ(later[1].start_ns, later[0].end_ns);
    CHECK_EQ(later[0].start_ns, 10'000'000);
}

TEST_CASE(bus_model_breaks_ties_deterministically)
{
    CanBusModel model(CanBusModelOptions{500'000, 8});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    const auto c = model.add_sender();
    // Same identifier: shorter payload first, then the lower bytes, then the lower sender.
    CHECK(model.submit(a, ext(0x10, 8, 9), at(0ms)));
    CHECK(model.submit(b, ext(0x10, 8, 9), at(0ms)));
    CHECK(model.submit(c, ext(0x10, 4, 9), at(0ms)));
    const auto first = run(model, SimTime{}, kForever);
    CHECK_EQ(first.size(), 3u);
    CHECK_EQ(first[0].sender, c);
    CHECK_EQ(first[1].sender, a);
    CHECK_EQ(first[2].sender, b);
    CHECK(model.submit(b, ext(0x10, 8, 1), at(1s)));
    CHECK(model.submit(a, ext(0x10, 8, 2), at(1s)));
    const auto second = run(model, SimTime{}, kForever);
    CHECK_EQ(second[0].sender, b);
}

TEST_CASE(bus_model_decides_only_transmissions_that_start_in_the_window)
{
    CanBusModel model(CanBusModelOptions{250'000, 8});
    const auto a = model.add_sender();
    for (int i = 0; i < 4; ++i)
    {
        CHECK(model.submit(a, ext(0x100 + static_cast<std::uint32_t>(i)), at(0ms)));
    }
    // Before 1 ms: frames starting at 0, 0.524 (it ends after 1 ms, but started before).
    const auto first = run(model, SimTime{}, at(1ms));
    CHECK_EQ(first.size(), 2u);
    CHECK_EQ(first[1].end_ns, 2 * kExt8At250k);
    CHECK_EQ(model.queued(a), 2u);
    // `from` delays the start: the bus is free at 1.048 ms, but nothing starts before 5 ms.
    const auto second = run(model, at(5ms), at(6ms));
    CHECK_EQ(second.size(), 2u);
    CHECK_EQ(second[0].start_ns, 5'000'000);
    CHECK_EQ(second[1].start_ns, 5'000'000 + kExt8At250k);
    // A transmission that would start exactly at `until` is left for later: a frame that becomes
    // ready at `until` may still be submitted and win.
    CHECK(model.submit(a, ext(0x500), at(10ms)));
    CHECK(run(model, SimTime{}, at(10ms)).empty());
    const auto b = model.add_sender();
    CHECK(model.submit(b, ext(0x100), at(10ms)));
    const auto third = run(model, SimTime{}, kForever);
    CHECK_EQ(third.size(), 2u);
    CHECK_EQ(third[0].id, 0x100u);
}

TEST_CASE(bus_model_lets_a_high_priority_frame_wait_only_for_the_frame_on_the_bus)
{
    // A floods the bus from 10 ms; B's priority-3 frame at 20 ms waits only for the frame that
    // is on the bus at 20 ms, then wins against A's queue.
    CanBusModel model(CanBusModelOptions{250'000, 64});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    for (int i = 0; i < 40; ++i)
    {
        CHECK(model.submit(a, ext(j1939_id(6, 0xFF10, 0x80), 8, static_cast<std::uint8_t>(i)), at(10ms)));
    }
    CHECK(model.submit(b, ext(j1939_id(3, 0xFF30, 0x90)), at(20ms)));
    const auto txs = run(model, SimTime{}, kForever);
    CHECK_EQ(txs.size(), 41u);
    // A's frame k runs [10 ms + k * 524 us, ...); frame 19 is on the bus at 20 ms.
    const std::int64_t busyUntil = 10'000'000 + 20 * kExt8At250k;
    CHECK_EQ(txs[20].sender, b);
    CHECK_EQ(txs[20].start_ns, busyUntil);
    CHECK_EQ(txs[20].end_ns, busyUntil + kExt8At250k);
    CHECK_EQ(txs[21].sender, a);
    CHECK_EQ(txs.back().end_ns, 10'000'000 + 41 * kExt8At250k);
}

TEST_CASE(bus_model_limits_each_queue_and_clears_a_sender)
{
    CanBusModel model(CanBusModelOptions{500'000, 2});
    const auto a = model.add_sender();
    const auto b = model.add_sender();
    CHECK(model.submit(a, ext(1), at(0ms)));
    CHECK(model.submit(a, ext(2), at(0ms)));
    CHECK(!model.submit(a, ext(3), at(0ms)));
    CHECK(model.submit(b, ext(4), at(0ms)));
    CHECK_EQ(model.queued(), 3u);
    CHECK_EQ(model.clear(a), 2u);
    CHECK_EQ(model.queued(a), 0u);
    const auto txs = run(model, SimTime{}, kForever);
    CHECK_EQ(txs.size(), 1u);
    CHECK_EQ(txs[0].sender, b);
    // Ring buffer reuse after wrapping.
    for (int round = 0; round < 5; ++round)
    {
        CHECK(model.submit(a, ext(10), at(1s)));
        CHECK(model.submit(a, ext(11), at(1s)));
        CHECK_EQ(run(model, SimTime{}, kForever).size(), 2u);
    }
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { static_cast<void>(model.submit(7, ext(1), at(0ms))); }));
    Frame invalid = ext(1);
    invalid.length = 9;
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { static_cast<void>(model.submit(a, invalid, at(0ms))); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { CanBusModel m(CanBusModelOptions{0, 1}); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { CanBusModel m(CanBusModelOptions{1, 0}); }));
}

TEST_CASE(bus_model_bitrate_change_applies_to_later_transmissions)
{
    CanBusModel model(CanBusModelOptions{250'000, 4});
    const auto a = model.add_sender();
    CHECK(model.submit(a, ext(1), at(0ms)));
    CHECK_EQ(run(model, SimTime{}, kForever)[0].end_ns, kExt8At250k);
    model.set_bitrate(500'000);
    CHECK_EQ(model.bitrate(), 500'000u);
    CHECK(model.submit(a, ext(1), at(1ms)));
    CHECK_EQ(run(model, SimTime{}, kForever)[0].end_ns, 1'000'000 + kExt8At250k / 2);
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([&] { model.set_bitrate(0); }));
}

// ---------------------------------------------------------------------------------------------
// The virtual bus with timing
// ---------------------------------------------------------------------------------------------

namespace {

VirtualCanBusOptions timed(std::uint32_t bitrate, SimDuration simulatorStep = {}, std::size_t queue = 64)
{
    VirtualCanBusOptions options;
    options.timing = VirtualCanBusTiming{bitrate, queue, simulatorStep};
    return options;
}

std::vector<TimedFrame> release(Port& port, SimTime now)
{
    std::vector<TimedFrame> out;
    port.release_before(now, [&](const TimedFrame& f) { out.push_back(f); });
    return out;
}

std::vector<PortEvent> release_events(Port& port, SimTime now)
{
    std::vector<PortEvent> out;
    port.release_events_before(now, [&](const PortEvent& e) { out.push_back(e); });
    return out;
}

} // namespace

TEST_CASE(virtual_bus_with_timing_delivers_at_the_end_of_each_transmission_in_arbitration_order)
{
    VirtualCanBus bus(timed(250'000));
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& r = bus.attach();
    CHECK(a.send({at(10ms), ext(j1939_id(6, 0xFF10, 0x80))}) == SendResult::Accepted);
    CHECK(a.send({at(10ms), ext(j1939_id(3, 0xFF20, 0x80))}) == SendResult::Accepted);
    CHECK(b.send({at(10ms), ext(j1939_id(3, 0xFF30, 0x90))}) == SendResult::Accepted);
    CHECK_EQ(a.transmit_pending(), 2u);
    // Nothing has ended before 10.5 ms.
    CHECK(release(r, at_us(10'500)).empty());
    const auto frames = release(r, at(20ms));
    CHECK_EQ(frames.size(), 3u);
    CHECK(frames[0].frame.id == j1939_id(3, 0xFF30, 0x90));
    CHECK_EQ(to_ns(frames[0].time), 10'000'000 + kExt8At250k);
    CHECK(frames[1].frame.id == j1939_id(6, 0xFF10, 0x80)); // a's FIFO: its first frame first
    CHECK_EQ(to_ns(frames[1].time), 10'000'000 + 2 * kExt8At250k);
    CHECK(frames[2].frame.id == j1939_id(3, 0xFF20, 0x80));
    CHECK_EQ(to_ns(frames[2].time), 10'000'000 + 3 * kExt8At250k);
    // The senders do not receive their own frames, but get the completions.
    CHECK_EQ(release(a, at(20ms)).size(), 1u);
    const auto events = release_events(a, at(20ms));
    CHECK_EQ(events.size(), 2u);
    CHECK(events[0].kind == PortEvent::Kind::TransmitCompleted);
    CHECK_EQ(events[0].id, j1939_id(6, 0xFF10, 0x80));
    CHECK_EQ(to_ns(events[0].time), 10'000'000 + 2 * kExt8At250k);
    CHECK(events[0].send_time == at(10ms));
    CHECK_EQ(to_ns(events[1].time), 10'000'000 + 3 * kExt8At250k);
    CHECK_EQ(release_events(b, at(20ms)).size(), 1u);
    CHECK(release_events(r, at(20ms)).empty());
    CHECK_EQ(bus.transmissions(), 3u);
    CHECK_EQ(a.transmit_pending(), 0u);
}

TEST_CASE(virtual_bus_with_timing_gives_the_same_times_whenever_the_receivers_look)
{
    // Exact model: the frame times depend only on the send times, not on how often the
    // receivers release frames (their step sizes).
    const auto scenario = [](const std::vector<SimDuration>& releaseSteps) {
        VirtualCanBus bus(timed(250'000));
        auto& a = bus.attach();
        auto& b = bus.attach();
        std::vector<VirtualCanBus::Endpoint*> receivers;
        for (std::size_t i = 0; i < releaseSteps.size(); ++i)
        {
            receivers.push_back(&bus.attach());
        }
        std::vector<std::vector<std::string>> traces(releaseSteps.size());
        std::vector<SimTime> next(releaseSteps.size(), SimTime{});
        for (SimTime t = SimTime{}; t <= at(60ms); t += 1ms)
        {
            // Receivers due at t release first (as in a step), then the senders send at t.
            for (std::size_t i = 0; i < receivers.size(); ++i)
            {
                if (next[i] == t)
                {
                    for (const auto& f : release(*receivers[i], t))
                    {
                        traces[i].push_back(std::to_string(to_ns(f.time)) + " " + std::to_string(f.frame.id));
                    }
                    next[i] += releaseSteps[i];
                }
            }
            if (t == at(10ms))
            {
                for (std::uint8_t k = 0; k < 20; ++k)
                {
                    CHECK(a.send({t, ext(j1939_id(6, 0xFF10, 0x80), 8, k)}) == SendResult::Accepted);
                }
            }
            if (t == at(15ms) || t == at(16ms))
            {
                CHECK(b.send({t, ext(j1939_id(2, 0xFF30, 0x90))}) == SendResult::Accepted);
                CHECK(b.send({t, std11(0x050, 2)}) == SendResult::Accepted);
            }
        }
        return traces;
    };
    const auto traces = scenario({1ms, 3ms, 7ms, 10ms, 20ms});
    CHECK_EQ(traces[0].size(), 24u);
    for (const auto& trace : traces)
    {
        CHECK(trace == traces[0]);
    }
    CHECK(scenario({1ms, 3ms, 7ms, 10ms, 20ms}) == traces);
}

namespace {

/// Sends a scripted list of frames at given times and records what it receives.
class Scripted final : public Steppable
{
public:
    explicit Scripted(std::vector<TimedFrame> script = {}) : script_(std::move(script)) {}

    void begin(SimTime now) override { now_ = now; }
    void receive(const TimedFrame& frame) override
    {
        trace_.push_back(std::to_string(to_ns(frame.time)) + " rx " + std::to_string(frame.frame.id) + " " +
                         std::to_string(frame.frame.data[0]));
    }
    void on_port_event(const PortEvent& event) override
    {
        trace_.push_back(std::to_string(to_ns(event.time)) + " done " + std::to_string(event.id));
    }
    void process() override {}
    void flush(Port& port) override
    {
        while (next_ < script_.size() && script_[next_].time <= now_)
        {
            TimedFrame f = script_[next_];
            f.time = now_;
            if (port.send(f) != SendResult::Accepted)
            {
                break; // try again at the next step
            }
            ++next_;
        }
    }

    [[nodiscard]] const std::vector<std::string>& trace() const { return trace_; }

private:
    std::vector<TimedFrame> script_;
    std::size_t next_ = 0;
    SimTime now_{};
    std::vector<std::string> trace_;
};

std::vector<TimedFrame> flood(SimTime t, std::uint32_t id, int count)
{
    std::vector<TimedFrame> out;
    for (int i = 0; i < count; ++i)
    {
        out.push_back({t, ext(id, 8, static_cast<std::uint8_t>(i))});
    }
    return out;
}

using Traces = std::vector<std::vector<std::string>>;

/// Three senders and two listeners on hosts with the given step sizes.
Traces run_hosts(const VirtualCanBusOptions& options, const std::vector<SimDuration>& steps)
{
    VirtualCanBus bus(options);
    std::vector<std::unique_ptr<Scripted>> nodes;
    nodes.push_back(std::make_unique<Scripted>(flood(at(10ms), j1939_id(6, 0xFF10, 0x10), 30)));
    auto second = flood(at(20ms), j1939_id(3, 0xFF20, 0x20), 3);
    second.push_back({at(30ms), std11(0x100, 1)});
    nodes.push_back(std::make_unique<Scripted>(second));
    nodes.push_back(std::make_unique<Scripted>(std::vector<TimedFrame>{{at(20ms), ext(j1939_id(3, 0xFF20, 0x08), 8, 7)},
                                                                       {at(30ms), ext(0x100u << 18, 1)}}));
    nodes.push_back(std::make_unique<Scripted>());
    nodes.push_back(std::make_unique<Scripted>());
    std::vector<std::unique_ptr<Host>> hosts;
    StepDriver driver;
    for (std::size_t i = 0; i < nodes.size(); ++i)
    {
        hosts.push_back(std::make_unique<Host>());
        hosts.back()->add(*nodes[i], bus.attach());
        driver.add(*hosts.back(), steps[i]);
    }
    driver.run_until(at(100ms));
    Traces traces;
    for (const auto& node : nodes)
    {
        // A host gets the events of a step before its frames; how they interleave depends on
        // the step size. Ordered by time (no two entries of one node have the same time), the
        // trace does not.
        auto trace = node->trace();
        std::stable_sort(trace.begin(), trace.end(),
                         [](const std::string& x, const std::string& y) { return std::stoll(x) < std::stoll(y); });
        traces.push_back(trace);
    }
    return traces;
}

} // namespace

TEST_CASE(virtual_bus_with_timing_does_not_depend_on_the_host_step_sizes)
{
    // The senders send at multiples of 10 ms, so their send times are the same with every step
    // size; the receivers (and the senders' own completions) see the same times.
    const Traces reference = run_hosts(timed(250'000), {10ms, 10ms, 10ms, 10ms, 10ms});
    CHECK_EQ(reference[3].size(), 30u + 4u + 2u);
    CHECK(reference[3] == reference[4]);
    for (const auto& steps : std::vector<std::vector<SimDuration>>{{1ms, 2ms, 5ms, 3ms, 7ms},
                                                                   {5ms, 10ms, 1ms, 1ms, 13ms},
                                                                   {2ms, 5ms, 10ms, 9ms, 1ms}})
    {
        CHECK(run_hosts(timed(250'000), steps) == reference);
    }
    // At 30 ms, the 11-bit frame 0x100 and the 29-bit frame with the same base identifier
    // compete: the 11-bit frame wins.
    const std::string std100 = " rx 256 0";
    const std::string ext100 = " rx " + std::to_string(0x100u << 18) + " 0";
    std::size_t posStd = 0;
    std::size_t posExt = 0;
    for (std::size_t i = 0; i < reference[3].size(); ++i)
    {
        if (reference[3][i].ends_with(std100))
        {
            posStd = i;
        }
        if (reference[3][i].ends_with(ext100))
        {
            posExt = i;
        }
    }
    CHECK(posStd != 0 && posExt != 0);
    CHECK(posStd < posExt);
}

TEST_CASE(virtual_bus_with_a_simulator_step_starts_frames_at_the_next_step_boundary)
{
    // Like the SIL Kit simple network simulator at 1 ms: a frame sent at 10 ms joins the bus at
    // the boundary 11 ms (the first after 10 ms).
    VirtualCanBus bus(timed(250'000, 1ms));
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& r = bus.attach();
    CHECK(a.send({at(10ms), ext(j1939_id(6, 0xFF10, 0x80))}) == SendResult::Accepted);
    CHECK(a.send({at(10ms), ext(j1939_id(3, 0xFF20, 0x80))}) == SendResult::Accepted);
    CHECK(b.send({at(10ms), ext(j1939_id(3, 0xFF30, 0x90))}) == SendResult::Accepted);
    const auto frames = release(r, at(20ms));
    CHECK_EQ(frames.size(), 3u);
    CHECK_EQ(to_ns(frames[0].time), 11'524'000);
    CHECK_EQ(to_ns(frames[1].time), 12'048'000);
    CHECK_EQ(to_ns(frames[2].time), 12'572'000);

    // The flood of the exact-model test: B's frame sent at 20 ms joins at 21 ms and waits for the
    // frame on the bus then.
    VirtualCanBus busy(timed(250'000, 1ms));
    auto& fa = busy.attach();
    auto& fb = busy.attach();
    auto& fr = busy.attach();
    for (std::uint8_t k = 0; k < 40; ++k)
    {
        CHECK(fa.send({at(10ms), ext(j1939_id(6, 0xFF10, 0x80), 8, k)}) == SendResult::Accepted);
    }
    CHECK(release(fr, at(20ms)).size() == 17u); // started at 11 ms, 524 us each
    CHECK(fb.send({at(20ms), ext(j1939_id(3, 0xFF30, 0x90))}) == SendResult::Accepted);
    const auto rest = release(fr, at(100ms));
    CHECK_EQ(rest.size(), 24u);
    std::int64_t bEnd = 0;
    for (const auto& f : rest)
    {
        if (f.frame.id == j1939_id(3, 0xFF30, 0x90))
        {
            bEnd = to_ns(f.time);
        }
    }
    // A's frame 19 runs [20.956, 21.480) ms; B follows it.
    CHECK_EQ(bEnd, 11'000'000 + 20 * kExt8At250k + kExt8At250k);
    CHECK_EQ(bEnd, 22'004'000);
}

TEST_CASE(virtual_bus_with_a_simulator_step_does_not_depend_on_the_receivers_step_sizes)
{
    const Traces reference = run_hosts(timed(250'000, 1ms), {10ms, 10ms, 10ms, 10ms, 10ms});
    CHECK(reference != run_hosts(timed(250'000), {10ms, 10ms, 10ms, 10ms, 10ms}));
    CHECK(run_hosts(timed(250'000, 1ms), {10ms, 10ms, 10ms, 1ms, 7ms}) == reference);
    CHECK(run_hosts(timed(250'000, 1ms), {5ms, 2ms, 1ms, 3ms, 13ms}) == reference);
    // A different simulator step changes the times (the approximation), deterministically.
    const Traces coarse = run_hosts(timed(250'000, 3ms), {10ms, 10ms, 10ms, 10ms, 10ms});
    CHECK(coarse != reference);
    CHECK(run_hosts(timed(250'000, 3ms), {1ms, 10ms, 5ms, 1ms, 7ms}) == coarse);
}

TEST_CASE(virtual_bus_with_timing_refuses_frames_when_the_transmit_queue_is_full)
{
    VirtualCanBus bus(timed(250'000, {}, 3));
    auto& a = bus.attach();
    auto& r = bus.attach();
    for (int i = 0; i < 3; ++i)
    {
        CHECK(a.send({at(0ms), ext(0x100)}) == SendResult::Accepted);
    }
    CHECK(a.send({at(0ms), ext(0x100)}) == SendResult::QueueFull);
    CHECK_EQ(a.transmit_pending(), 3u);
    // At 0.6 ms one frame has ended and one is on the bus: one waits, room for two.
    CHECK_EQ(release(r, at_us(600)).size(), 1u);
    CHECK_EQ(a.transmit_pending(), 1u);
    CHECK(a.send({at_us(600), ext(0x100)}) == SendResult::Accepted);
    CHECK(a.send({at_us(600), ext(0x100)}) == SendResult::Accepted);
    CHECK(a.send({at_us(600), ext(0x100)}) == SendResult::QueueFull);
    // A duplicate needs two places.
    CHECK_EQ(release(r, at(10ms)).size(), 4u);
    a.set_fault_injector([](const TimedFrame&) { return FaultAction::duplicate(); });
    CHECK(a.send({at(10ms), ext(0x100)}) == SendResult::Accepted);
    CHECK(a.send({at(10ms), ext(0x100)}) == SendResult::QueueFull);
}

TEST_CASE(virtual_bus_with_timing_applies_faults_on_the_bus)
{
    VirtualCanBus bus(timed(250'000));
    auto& a = bus.attach();
    auto& r = bus.attach();
    int n = 0;
    a.set_fault_injector([&n](const TimedFrame&) {
        switch (n++)
        {
        case 0: return FaultAction::drop();
        case 1: return FaultAction::duplicate();
        case 2: return FaultAction::delay_by(5ms);
        case 3: return FaultAction::queue_full();
        case 4: return FaultAction::failed();
        default: return FaultAction::deliver();
        }
    });
    CHECK(a.send({at(0ms), ext(1, 8, 0)}) == SendResult::Accepted);  // dropped: takes the bus, reaches no one
    CHECK(a.send({at(0ms), ext(2, 8, 1)}) == SendResult::Accepted);  // twice
    CHECK(a.send({at(0ms), ext(3, 8, 2)}) == SendResult::Accepted);  // ready at 5 ms
    CHECK(a.send({at(0ms), ext(4, 8, 3)}) == SendResult::QueueFull); // not taken
    CHECK(a.send({at(0ms), ext(5, 8, 4)}) == SendResult::Failed);    // not taken
    CHECK(a.send({at(0ms), ext(6, 8, 5)}) == SendResult::Accepted);  // overtakes the delayed frame
    const auto frames = release(r, at(10ms));
    CHECK_EQ(frames.size(), 4u);
    CHECK_EQ(frames[0].frame.id, 2u);
    CHECK_EQ(to_ns(frames[0].time), 2 * kExt8At250k);
    CHECK_EQ(frames[1].frame.id, 2u);
    CHECK_EQ(to_ns(frames[1].time), 3 * kExt8At250k);
    CHECK_EQ(frames[2].frame.id, 6u);
    CHECK_EQ(to_ns(frames[2].time), 4 * kExt8At250k);
    CHECK_EQ(frames[3].frame.id, 3u);
    CHECK_EQ(to_ns(frames[3].time), 5'000'000 + kExt8At250k);
    // The sender learns of every transmission, also the dropped one.
    const auto events = release_events(a, at(10ms));
    CHECK_EQ(events.size(), 5u);
    CHECK_EQ(events[0].id, 1u);
    CHECK(events[4].send_time == at(0ms));
    CHECK_EQ(to_ns(events[4].time), 5'000'000 + kExt8At250k);
}

TEST_CASE(virtual_bus_with_timing_refuses_frames_older_than_the_decided_bus)
{
    VirtualCanBus bus(timed(500'000));
    auto& a = bus.attach();
    auto& r = bus.attach();
    CHECK(release(r, at(10ms)).empty());
    CHECK(vehicle_sil::unit::throws<std::logic_error>([&] { static_cast<void>(a.send({at(9ms), ext(1)})); }));
    CHECK(a.send({at(10ms), ext(1)}) == SendResult::Accepted);

    // With a simulator step the bus is decided up to the last boundary before `now`.
    VirtualCanBus stepped(timed(500'000, 2ms));
    auto& sa = stepped.attach();
    auto& sr = stepped.attach();
    CHECK(release(sr, at(9ms)).empty()); // boundaries up to 8 ms are decided
    CHECK(vehicle_sil::unit::throws<std::logic_error>([&] { static_cast<void>(sa.send({at_us(7999), ext(1)})); }));
    CHECK(sa.send({at(8ms), ext(1)}) == SendResult::Accepted);
    CHECK_EQ(release(sr, at(11ms)).size(), 1u);
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] {
        VirtualCanBus b(timed(500'000, -1ms));
    }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { VirtualCanBus b(timed(0)); }));
    CHECK(vehicle_sil::unit::throws<std::invalid_argument>([] { VirtualCanBus b(timed(500'000, {}, 0)); }));
}

TEST_CASE(virtual_bus_without_timing_reports_no_events)
{
    VirtualCanBus bus;
    auto& a = bus.attach();
    auto& r = bus.attach();
    CHECK(a.send({at(1ms), ext(1)}) == SendResult::Accepted);
    CHECK_EQ(release(r, at(2ms)).size(), 1u);
    CHECK(release_events(a, at(2ms)).empty());
    CHECK_EQ(bus.transmissions(), 0u);
    CHECK_EQ(a.transmit_pending(), 0u);
}

TEST_CASE(host_hands_port_events_to_the_steppable_before_the_frames)
{
    VirtualCanBus bus(timed(500'000));
    Scripted a({{at(0ms), ext(1)}});
    Scripted b({{at(0ms), ext(2)}});
    Host ha;
    Host hb;
    ha.add(a, bus.attach());
    hb.add(b, bus.attach());
    StepDriver driver;
    driver.add(ha, 1ms);
    driver.add(hb, 1ms);
    driver.run_until(at(3ms));
    // 29-bit, 8 bytes at 500 kbit/s: 262 us each; id 1 first.
    CHECK(a.trace() == (std::vector<std::string>{"262000 done 1", "524000 rx 2 0"}));
    CHECK(b.trace() == (std::vector<std::string>{"524000 done 2", "262000 rx 1 0"}));
}

TEST_CASE(virtual_bus_with_timing_lets_a_frame_sent_when_the_bus_frees_up_compete)
{
    // The bus becomes free at exactly 10 ms. A host releasing at 10 ms must not decide the
    // transmission that starts then: another host sends a higher-priority frame at 10 ms.
    VirtualCanBus bus(timed(250'000));
    auto& a = bus.attach();
    auto& b = bus.attach();
    auto& r = bus.attach();
    CHECK(a.send({at(10ms) - std::chrono::nanoseconds(kExt8At250k), ext(0x900)}) == SendResult::Accepted);
    CHECK(a.send({at(10ms) - std::chrono::nanoseconds(kExt8At250k), ext(0x800)}) == SendResult::Accepted);
    CHECK(release(r, at(10ms)).size() == 0u); // the first ends at 10 ms: not before 10 ms
    CHECK(b.send({at(10ms), ext(0x100)}) == SendResult::Accepted);
    const auto frames = release(r, at(20ms));
    CHECK_EQ(frames.size(), 3u);
    CHECK_EQ(frames[0].frame.id, 0x900u);
    CHECK_EQ(frames[1].frame.id, 0x100u);
    CHECK_EQ(frames[2].frame.id, 0x800u);
}
