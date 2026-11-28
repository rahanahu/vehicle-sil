// SPDX-License-Identifier: MIT
//
// Deterministic unit tests for examples/standard_silkit_participant/rx_inbox.hpp.
// Exercises strict-before filtering (tx_ns < now_ns), non-dropping retention of equal-or-later
// timestamp frames (tx_ns >= now_ns), and canonical ordering independent of arrival order.

#include "rx_inbox.hpp"
#include "test_harness.hpp"

#include <cstdint>
#include <cstring>
#include <vector>

namespace {

using standard_silkit_participant::DeterministicRxInbox;
using standard_silkit_participant::RxRecord;

RxRecord make_record(std::int64_t tx_ns, std::uint32_t can_id, std::uint32_t flags,
                     std::initializer_list<std::uint8_t> bytes)
{
    RxRecord r;
    r.tx_ns = tx_ns;
    r.can_id = can_id;
    r.flags = flags;
    r.length = bytes.size();
    r.dlc = static_cast<std::uint16_t>(bytes.size());
    std::size_t i = 0;
    for (std::uint8_t b : bytes)
    {
        r.data[i++] = b;
    }
    return r;
}

} // namespace

TEST_CASE(rx_inbox_strict_before_defers_equal_and_future_without_dropping)
{
    DeterministicRxInbox inbox;
    inbox.push(make_record(30'000'000, 0x100u, 0u, {0x11, 0x00, 0x03, 0x00}));
    inbox.push(make_record(40'000'000, 0x100u, 0u, {0x11, 0x00, 0x04, 0x00}));
    // Arrived before step 50 ms executes, with timestamp == 50 ms (simultaneous boundary) and > 50 ms.
    inbox.push(make_record(50'000'000, 0x100u, 0u, {0x11, 0x00, 0x05, 0x00}));
    inbox.push(make_record(60'000'000, 0x100u, 0u, {0x11, 0x00, 0x06, 0x00}));

    std::uint64_t deferred_same_ts = 0;
    const auto step_50 = inbox.drain_before(50'000'000, &deferred_same_ts);
    CHECK_EQ(step_50.size(), std::size_t{2});
    CHECK_EQ(step_50[0].tx_ns, std::int64_t{30'000'000});
    CHECK_EQ(step_50[1].tx_ns, std::int64_t{40'000'000});
    CHECK_EQ(deferred_same_ts, std::uint64_t{1});
    CHECK_EQ(inbox.queued_count(), std::size_t{2});

    // Equal-timestamp frame (50 ms) and later frame (60 ms) MUST NOT be dropped; both drain at 75 ms.
    const auto step_75 = inbox.drain_before(75'000'000, &deferred_same_ts);
    CHECK_EQ(step_75.size(), std::size_t{2});
    CHECK_EQ(step_75[0].tx_ns, std::int64_t{50'000'000});
    CHECK_EQ(step_75[1].tx_ns, std::int64_t{60'000'000});
    CHECK_EQ(deferred_same_ts, std::uint64_t{1});
    CHECK_EQ(inbox.queued_count(), std::size_t{0});
}

TEST_CASE(rx_inbox_canonical_sort_order_independent_of_arrival)
{
    const RxRecord r0 = make_record(10'000'000, 0x100u, 0u, {0x01});
    const RxRecord r1 = make_record(20'000'000, 0x100u, 0u, {}); // 0-byte payload
    const RxRecord r2 = make_record(20'000'000, 0x100u, 0u, {0x01, 0x00});
    const RxRecord r3 = make_record(20'000'000, 0x100u, 0u, {0x01, 0x02});
    const RxRecord r4 = make_record(20'000'000, 0x100u, 1u, {0x00}); // higher flags
    const RxRecord r5 = make_record(20'000'000, 0x101u, 0u, {0x00}); // higher can_id

    // Push in scrambled / reverse order.
    DeterministicRxInbox inbox_a;
    for (const auto& rec : {r5, r3, r1, r4, r2, r0})
    {
        inbox_a.push(rec);
    }

    // Push in a different scrambled order.
    DeterministicRxInbox inbox_b;
    for (const auto& rec : {r2, r0, r4, r5, r1, r3})
    {
        inbox_b.push(rec);
    }

    const auto out_a = inbox_a.drain_before(25'000'000);
    const auto out_b = inbox_b.drain_before(25'000'000);

    CHECK_EQ(out_a.size(), std::size_t{6});
    CHECK_EQ(out_b.size(), std::size_t{6});

    const std::vector<RxRecord> expected = {r0, r1, r2, r3, r4, r5};
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        CHECK_EQ(out_a[i].tx_ns, expected[i].tx_ns);
        CHECK_EQ(out_a[i].can_id, expected[i].can_id);
        CHECK_EQ(out_a[i].flags, expected[i].flags);
        CHECK_EQ(out_a[i].length, expected[i].length);
        CHECK(out_a[i].length == 0u || std::memcmp(out_a[i].data.data(), expected[i].data.data(), out_a[i].length) == 0);

        CHECK_EQ(out_b[i].tx_ns, expected[i].tx_ns);
        CHECK_EQ(out_b[i].can_id, expected[i].can_id);
        CHECK_EQ(out_b[i].flags, expected[i].flags);
        CHECK_EQ(out_b[i].length, expected[i].length);
        CHECK(out_b[i].length == 0u || std::memcmp(out_b[i].data.data(), expected[i].data.data(), out_b[i].length) == 0);
    }
}

TEST_CASE(rx_inbox_asymmetric_simultaneous_boundaries_preserve_all_frames)
{
    // Replay a 10 ms sender against a 25 ms receiver where simultaneous-boundary frames
    // (0, 50, 100 ms) arrive BEFORE the 25 ms step handler runs.
    DeterministicRxInbox inbox;
    std::uint64_t deferred_same_ts = 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> processed; // (tx_ms, rx_step_ms)

    std::int64_t next_tx_ms = 0;
    for (std::int64_t step_ms = 0; step_ms <= 125; step_ms += 25)
    {
        while (next_tx_ms <= step_ms)
        {
            inbox.push(make_record(next_tx_ms * 1'000'000LL, 0x100u, 0u, {0x11}));
            next_tx_ms += 10;
        }
        const auto ready = inbox.drain_before(step_ms * 1'000'000LL, &deferred_same_ts);
        for (const auto& rec : ready)
        {
            CHECK(rec.tx_ns < step_ms * 1'000'000LL);
            processed.emplace_back(rec.tx_ns / 1'000'000LL, step_ms);
        }
    }

    // 0 ms, 50 ms, and 100 ms were all deferred on their simultaneous step.
    CHECK_EQ(deferred_same_ts, std::uint64_t{3});
    // Every frame sent at 0, 10, ..., 120 ms (13 frames) was processed strictly after tx_ms.
    CHECK_EQ(processed.size(), std::size_t{13});
    CHECK_EQ(inbox.queued_count(), std::size_t{0});

    // Spot-check the simultaneous boundaries: 0 -> 25, 50 -> 75, 100 -> 125.
    CHECK_EQ(processed[0].first, std::int64_t{0});
    CHECK_EQ(processed[0].second, std::int64_t{25});
    CHECK_EQ(processed[5].first, std::int64_t{50});
    CHECK_EQ(processed[5].second, std::int64_t{75});
    CHECK_EQ(processed[10].first, std::int64_t{100});
    CHECK_EQ(processed[10].second, std::int64_t{125});
}
