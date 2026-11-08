// SPDX-License-Identifier: MIT
//
// The LIN cluster that the example master and slave peers form, and helpers they share.
//
//   0x10  command  2 bytes  published by the master: the header time in ms, little endian
//   0x20  echo     2 bytes  published by the "echo" slave: the newest command it received
//   0x21  status   1 byte   published by the "status" slave: commands received so far, mod 256
//
// Trace records (see examples/virtual_time/common/vt_common.hpp for the format):
//   master: result t_us=.. id=0x.. status=ok|no_response|error data=..
//           state t_us=.. state=operational|sleep
//   slave:  frame t_us=.. rx_us=.. id=0x.. data=..   (rx_us: the step that handled the frame)
//           state t_us=.. state=operational|sleep
//           wakeup t_us=..   (this slave sent a wakeup pulse)
#pragma once

#include "../../virtual_time/common/vt_common.hpp"

#include "vehicle_sil/lin/lin_schedule.hpp"
#include "vehicle_sil/lin/lin_types.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <string>

namespace vehicle_sil::lin_example {

using lin::LinChecksumModel;
using lin::LinId;
using lin::LinPublisher;

inline constexpr LinId kCommandId = 0x10;
inline constexpr LinId kEchoId = 0x20;
inline constexpr LinId kStatusId = 0x21;

inline lin::LinScheduleTable cluster_schedule(SimDuration slot)
{
    return {{kCommandId, 2, LinChecksumModel::Enhanced, slot, LinPublisher::Master},
            {kEchoId, 2, LinChecksumModel::Enhanced, slot, LinPublisher::Slave},
            {kStatusId, 1, LinChecksumModel::Enhanced, slot, LinPublisher::Slave}};
}

inline std::array<std::uint8_t, 2> command_at(SimTime t)
{
    const auto v = static_cast<std::uint16_t>(t.time_since_epoch() / std::chrono::milliseconds(1));
    return {static_cast<std::uint8_t>(v & 0xFFu), static_cast<std::uint8_t>(v >> 8)};
}

/// Simulation times from a comma-separated list of milliseconds (--key 100,300); empty if absent.
inline std::set<SimTime> times_ms(const cli::Args& args, const std::string& key)
{
    std::set<SimTime> out;
    if (!args.has(key))
    {
        return out;
    }
    for (const auto& item : args.list(key))
    {
        std::size_t used = 0;
        unsigned long long value = 0;
        try
        {
            value = std::stoull(item, &used);
        }
        catch (const std::exception&)
        {
            used = 0;
        }
        if (used != item.size() || value > cli::Args::kMaxMillis)
        {
            throw cli::UsageError("invalid time in --" + key + ": " + item);
        }
        out.insert(SimTime{} + std::chrono::milliseconds(value));
    }
    return out;
}

} // namespace vehicle_sil::lin_example
