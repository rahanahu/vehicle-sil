// SPDX-License-Identifier: MIT
//
// Small helpers shared by the J1939 example peers: command line parsing, exit codes and the
// test vectors both peers agree on.
#pragma once

#include "vehicle_sil/cli/args.hpp"
#include "vehicle_sil/cli/console.hpp"

#include <array>
#include <cstdint>

namespace vehicle_sil::j1939_example {

using cli::Args;
using cli::log;
using cli::UsageError;

/// Process exit codes (vehicle_sil::cli::ExitCode). The integration test launcher checks these.
enum ExitCode : int
{
    kExitOk = cli::kExitOk,
    kExitVerificationFailed = cli::kExitFailed, ///< A frame did not match expectations.
    kExitTimeout = cli::kExitTimeout,           ///< Expected traffic did not arrive in time.
    kExitSetupFailed = cli::kExitSetupFailed,   ///< Could not connect or start SIL Kit.
    kExitUsage = cli::kExitUsage,               ///< Bad command line.
};

/// Payload of the n-th test message (n starts at 0). Both peers derive the payload from the
/// sequence number so the receiver can check every byte.
[[nodiscard]] inline std::array<std::uint8_t, 8> test_payload(std::uint32_t sequence) noexcept
{
    const auto seq = static_cast<std::uint8_t>(sequence);
    return {seq, 0xA5, 0x5A, static_cast<std::uint8_t>(seq ^ 0xFFu), 0x01, 0x23, 0x45, static_cast<std::uint8_t>(0x67u + seq)};
}

} // namespace vehicle_sil::j1939_example
