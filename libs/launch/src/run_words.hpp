// SPDX-License-Identifier: MIT
//
// The words of the run's output that more than one part of it uses: how a process ended, a
// time in milliseconds. Pure functions. Internal.
#pragma once

#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace vehicle_sil::launch::detail {

/// The launcher's own lines: the participant name of its "log" events, the tag of its lines.
inline constexpr std::string_view kLauncherName = "vehicle_sil_launch";
inline constexpr std::string_view kLauncherTag = "[vehicle_sil_launch] ";

inline constexpr std::int64_t kNanosPerMilli = 1'000'000;

inline std::string hex_code(int code)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(code));
    return text;
}


/// "exited with code 7", "was ended by signal SIGSEGV".
inline std::string exit_text(const process::ExitStatus& exit)
{
    if (exit.signal)
    {
        return "was ended by signal " + process::signal_name(*exit.signal);
    }
    // A Windows NTSTATUS (0xC0000005 an access violation, 0xC000013A Ctrl+C) reads better in hex.
    return "exited with code " + (exit.code < 0 ? hex_code(exit.code) : std::to_string(exit.code));
}

inline bool ended_by_itself(const ParticipantRun& p)
{
    return p.ending == Ending::Exited || p.ending == Ending::ExitedBeforeStart;
}

inline bool failed_exit(const ParticipantRun& p)
{
    return p.exit && ended_by_itself(p) && (p.exit->code != 0 || p.exit->signal);
}

/// Nanoseconds as milliseconds in the shortest form ("10", "2.5").
inline std::string millis_text(std::int64_t ns)
{
    return speed_text(static_cast<double>(ns) / static_cast<double>(kNanosPerMilli));
}

} // namespace vehicle_sil::launch::detail
