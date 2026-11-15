// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/nothing_came_hint.hpp"

#include <utility>

namespace vehicle_sil::bridge_runtime {

NothingCameHint::NothingCameHint(std::function<std::string()> text, std::chrono::milliseconds after) :
  text_(std::move(text)),
  after_(after)
{
}

std::optional<std::string> NothingCameHint::look(std::uint64_t sent, std::uint64_t received,
                                                 std::chrono::steady_clock::time_point now)
{
    if (done_)
    {
        return std::nullopt;
    }
    if (received > 0)
    {
        done_ = true; // a device answers: whatever comes later, the bus is there
        return std::nullopt;
    }
    if (sent == 0)
    {
        return std::nullopt;
    }
    if (!first_sent_)
    {
        first_sent_ = now;
    }
    if (now - *first_sent_ < after_)
    {
        return std::nullopt;
    }
    done_ = true;
    return text_();
}

} // namespace vehicle_sil::bridge_runtime
