// SPDX-License-Identifier: MIT
//
// The words of the run's output that more than one part of it uses: how a process ended (a time in
// milliseconds is in check_words.hpp). Pure functions. Internal.
#pragma once

#include "vehicle_sil/runner/run_outcome.hpp"
#include "vehicle_sil/process/child_process.hpp"

#include "vehicle_sil/config/detail/check_words.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace vehicle_sil::runner::detail {

/// The launcher's own lines: the participant name of its "log" events, the tag of its lines.
inline constexpr std::string_view kLauncherName = "vehicle_sil_launch";
inline constexpr std::string_view kLauncherTag = "[vehicle_sil_launch] ";

inline std::string hex_code(int code)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(code));
    return text;
}


/// An exit code as the outputs show it. A Windows NTSTATUS (0xC0000005 an access violation) reads
/// better in hex; 0xC000013A (STATUS_CONTROL_C_EXIT), 0xC0000135 (STATUS_DLL_NOT_FOUND) and
/// 0xC0000139 (STATUS_ENTRYPOINT_NOT_FOUND) say nothing to a user on their own, so each carries a
/// parenthesized explanation that fits after both "exit code " and "exited with code ".
inline std::string exit_code_text(int code)
{
    if (code >= 0)
    {
        return std::to_string(code);
    }
    switch (static_cast<std::uint32_t>(code))
    {
    case 0xC000013Au:
        return hex_code(code) + " (Windows ended it on a Ctrl+C, a Ctrl+Break or a closed console)";
    case config::detail::kStatusDllNotFound:
        return hex_code(code) + " (Windows did not find a DLL that it needs)";
    case config::detail::kStatusEntryPointNotFound:
        return hex_code(code) + " (a DLL that it needs is of another version: a function in it is missing)";
    default:
        return hex_code(code);
    }
}

/// "exited with code 7", "was ended by signal SIGSEGV".
inline std::string exit_text(const process::ExitStatus& exit)
{
    if (exit.signal)
    {
        return "was ended by signal " + process::signal_name(*exit.signal);
    }
    return "exited with code " + exit_code_text(exit.code);
}

inline bool ended_by_itself(const ParticipantRun& p)
{
    return p.ending == Ending::Exited || p.ending == Ending::ExitedBeforeStart;
}

inline bool failed_exit(const ParticipantRun& p)
{
    return p.exit && ended_by_itself(p) && (p.exit->code != 0 || p.exit->signal);
}

} // namespace vehicle_sil::runner::detail
