// SPDX-License-Identifier: MIT
//
// The fake link behaves like the backends the bridge expects: events from its own thread in
// order, nothing after stop(), what stop() reports, a fault that ends the frames.
#include "check_text.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::eth_link;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::FakeLink;
using vehicle_sil::fake_eth::FakeSegment;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::unit::eventually;

const char* const kEcu = "02:00:00:00:00:01";
const char* const kOutside = "02:00:00:00:00:99";

/// What a handler received.
struct Recorder
{
    mutable std::mutex mutex;
    std::vector<LinkEventKind> kinds;
    std::vector<Bytes> frames;
    std::vector<std::string> details;
    std::uint64_t overruns = 0;
    std::vector<std::thread::id> threads;

    ExternalLink::EventHandler handler()
    {
        return [this](const LinkEvent& event) {
            const std::lock_guard lock(mutex);
            kinds.push_back(event.kind);
            threads.push_back(std::this_thread::get_id());
            if (event.kind == LinkEventKind::Frame)
            {
                frames.emplace_back(event.frame.begin(), event.frame.end());
            }
            if (event.kind == LinkEventKind::Overrun)
            {
                overruns += event.count;
            }
            if (!event.detail.empty())
            {
                details.emplace_back(event.detail);
            }
        };
    }

    std::size_t frame_count() const
    {
        const std::lock_guard lock(mutex);
        return frames.size();
    }
};

} // namespace

TEST_CASE(fake_link_delivers_in_order_from_its_own_thread)
{
    FakeSegment segment;
    FakeLink link(segment, "fake0");
    Recorder recorder;
    link.start(recorder.handler());
    for (std::uint8_t i = 0; i < 50; ++i)
    {
        segment.send(make_frame(kEcu, kOutside, 0x88B5, 46, i));
    }
    CHECK(eventually([&] { return recorder.frame_count() == 50; }, 5000ms));
    link.stop();
    const std::lock_guard lock(recorder.mutex);
    for (std::uint8_t i = 0; i < 50; ++i)
    {
        CHECK_EQ(recorder.frames[i][14], i);
        CHECK(recorder.threads[i] != std::this_thread::get_id());
    }
}

TEST_CASE(fake_link_transmits_to_the_other_links_and_echoes_only_when_asked)
{
    FakeSegment segment;
    FakeLink a(segment, "a");
    FakeLink b(segment, "b");
    Recorder ra;
    Recorder rb;
    a.start(ra.handler());
    b.start(rb.handler());
    const auto frame = make_frame(kOutside, kEcu, 0x88B5);
    CHECK(a.transmit(frame) == TransmitResult::Ok);
    CHECK(eventually([&] { return rb.frame_count() == 1 && segment.idle(); }, 5000ms));
    CHECK_EQ(ra.frame_count(), std::size_t{0});
    a.set_echo_own(true);
    CHECK(a.transmit(frame) == TransmitResult::Ok);
    CHECK(eventually([&] { return ra.frame_count() == 1 && rb.frame_count() == 2; }, 5000ms));
    CHECK_EQ(segment.transmitted().size(), std::size_t{2});
    CHECK_EQ(a.transmitted().size(), std::size_t{2});
    a.set_transmit_result(TransmitResult::QueueFull);
    CHECK(a.transmit(frame) == TransmitResult::QueueFull);
    CHECK_EQ(segment.transmitted().size(), std::size_t{2});
}

TEST_CASE(fake_link_stop_reports_overruns_and_then_nothing)
{
    FakeSegment segment;
    FakeLink link(segment, "fake0");
    Recorder recorder;
    link.set_overruns_at_stop(3);
    link.start(recorder.handler());
    link.report_overrun(2);
    CHECK(eventually([&] { return link.idle(); }, 5000ms));
    link.stop();
    {
        const std::lock_guard lock(recorder.mutex);
        CHECK_EQ(recorder.overruns, std::uint64_t{5});
        CHECK(recorder.threads.back() == std::this_thread::get_id()); // reported by stop() itself
    }
    segment.send(make_frame(kEcu, kOutside, 0x88B5));
    CHECK(link.transmit(make_frame(kOutside, kEcu, 0x88B5)) == TransmitResult::Failed);
    CHECK_EQ(recorder.frame_count(), std::size_t{0});
    link.stop(); // twice is fine
    CHECK_EQ(link.stop_calls(), 2);
}

TEST_CASE(fake_link_fault_ends_the_frames_and_transmit_fails)
{
    FakeSegment segment;
    FakeLink link(segment, "fake0");
    Recorder recorder;
    link.start(recorder.handler());
    link.report_fault("interface fake0 disappeared");
    segment.send(make_frame(kEcu, kOutside, 0x88B5));
    CHECK(eventually([&] { return link.idle(); }, 5000ms));
    CHECK(link.transmit(make_frame(kOutside, kEcu, 0x88B5)) == TransmitResult::Failed);
    const std::lock_guard lock(recorder.mutex);
    CHECK_EQ(recorder.frames.size(), std::size_t{0});
    CHECK_EQ(recorder.details.size(), std::size_t{1});
    CHECK_TEXT(recorder.details.front(), "interface fake0 disappeared");
}

TEST_CASE(fake_link_start_failure_is_a_link_error)
{
    FakeSegment segment;
    FakeLink link(segment, "fake0");
    link.fail_start(LinkErrorKind::NoSuchInterface, "no interface 'fake0'");
    CHECK(vehicle_sil::unit::throws_if<LinkError>([&] { link.start([](const LinkEvent&) {}); },
                                                  [](const LinkError& e) { return e.kind() == LinkErrorKind::NoSuchInterface; }));
    CHECK(!link.started());
    CHECK_TEXT(link.describe(), "fake 'fake0' (virtual)");
    CHECK_TEXT(to_string(LinkErrorKind::PermissionDenied), "permission denied");
    CHECK_TEXT(to_string(LinkState::Down), "link down");
}
