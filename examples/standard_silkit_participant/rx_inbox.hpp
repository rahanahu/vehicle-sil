// SPDX-License-Identifier: MIT
//
// Zero-vehicle-sil-dependency deterministic CAN RX inbox for standard SIL Kit participants.
//
// Buffers frames delivered by SIL Kit's asynchronous AddFrameHandler callback and drains ONLY
// frames sent strictly before the current simulation step (tx_ns < now_ns). Frames with
// tx_ns >= now_ns are never dropped; they remain queued for a later step. Drained frames are
// sorted canonically so processing order does not depend on IPC arrival order.

#ifndef STANDARD_SILKIT_PARTICIPANT_RX_INBOX_HPP
#define STANDARD_SILKIT_PARTICIPANT_RX_INBOX_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

namespace standard_silkit_participant {

struct RxRecord
{
    std::int64_t tx_ns = 0;
    std::uint32_t can_id = 0;
    std::uint32_t flags = 0;
    std::uint16_t dlc = 0;
    std::size_t length = 0;
    std::array<std::uint8_t, 64> data{};
};

class DeterministicRxInbox
{
public:
    explicit DeterministicRxInbox(std::size_t initial_capacity = 256u)
    {
        inbox_.reserve(initial_capacity);
    }

    void push(const RxRecord& rec)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        inbox_.push_back(rec);
    }

    // Drain only frames sent strictly before now_ns (tx_ns < now_ns), keeping equal-or-later
    // timestamp frames queued for a subsequent step, and sort drained frames canonically.
    std::vector<RxRecord> drain_before(std::int64_t now_ns, std::uint64_t* deferred_same_ts = nullptr)
    {
        std::vector<RxRecord> ready;
        std::uint64_t same_ts_count = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::vector<RxRecord> deferred;
            deferred.reserve(inbox_.size());
            for (const auto& rec : inbox_)
            {
                if (rec.tx_ns < now_ns)
                {
                    ready.push_back(rec);
                }
                else
                {
                    if (rec.tx_ns == now_ns)
                    {
                        ++same_ts_count;
                    }
                    deferred.push_back(rec);
                }
            }
            inbox_.swap(deferred);
        }

        if (deferred_same_ts != nullptr)
        {
            *deferred_same_ts += same_ts_count;
        }

        std::sort(ready.begin(), ready.end(), [](const RxRecord& a, const RxRecord& b) {
            if (a.tx_ns != b.tx_ns)
            {
                return a.tx_ns < b.tx_ns;
            }
            if (a.can_id != b.can_id)
            {
                return a.can_id < b.can_id;
            }
            if (a.flags != b.flags)
            {
                return a.flags < b.flags;
            }
            if (a.length != b.length)
            {
                return a.length < b.length;
            }
            return a.length > 0u && std::memcmp(a.data.data(), b.data.data(), a.length) < 0;
        });

        return ready;
    }

    std::size_t queued_count() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return inbox_.size();
    }

private:
    mutable std::mutex mutex_;
    std::vector<RxRecord> inbox_;
};

} // namespace standard_silkit_participant

#endif // STANDARD_SILKIT_PARTICIPANT_RX_INBOX_HPP
