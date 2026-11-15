// SPDX-License-Identifier: MIT
//
// The hint of a bridge that sends to a real bus and hears nothing back: the bridge has sent frames
// out for a while, and not one frame has come from a device there. A cable that is not plugged in,
// a device that is off, the wrong adapter or channel all look like this, and sending succeeds all
// the same. Each bridge says it in its own words; the rule is one.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace vehicle_sil::bridge_runtime {

/// How long after the first frame sent out the hint is due.
inline constexpr std::chrono::seconds kNothingCameAfter{10};

/// Decides when the hint is due, from the bridge's counts. No clock and no thread of its own:
/// the caller gives the time with the counts, from one thread (run_bridge()'s hooks).
class NothingCameHint
{
public:
    /// `text`: the hint, asked for when it is due (it may say what the bridge learned meanwhile).
    explicit NothingCameHint(std::function<std::string()> text, std::chrono::milliseconds after = kNothingCameAfter);

    /// `sent`: frames the bridge sent out, `received`: frames that came from devices outside,
    /// both since the start; `now`: when they were read. The hint, once: when `after` or more has
    /// passed since the first look that saw `sent` above 0, and `received` is still 0. Once a
    /// look saw `received` above 0, or the hint was given, it says nothing more.
    [[nodiscard]] std::optional<std::string> look(std::uint64_t sent, std::uint64_t received,
                                                  std::chrono::steady_clock::time_point now);

private:
    std::function<std::string()> text_;
    std::chrono::milliseconds after_;
    std::optional<std::chrono::steady_clock::time_point> first_sent_;
    bool done_ = false;
};

} // namespace vehicle_sil::bridge_runtime
