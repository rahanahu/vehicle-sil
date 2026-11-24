// SPDX-License-Identifier: MIT
//
// Pure C++20 FIPS 180-4 SHA-256 over raw byte buffers (no external crypto dependencies). Internal.
#pragma once

#include <string>
#include <string_view>

namespace vehicle_sil::launch::detail {

/// Computes lowercase hexadecimal FIPS 180-4 SHA-256 over the exact raw bytes of `data`.
[[nodiscard]] std::string sha256_hex(std::string_view data);

} // namespace vehicle_sil::launch::detail
