// SPDX-License-Identifier: MIT
//
// Text for the messages of ecu_main() (vehicle_sil/simulation/ecu_main.hpp), shared by its parts.
// Internal to the library; not installed, not part of the API.
#pragma once

#include <string>
#include <string_view>

namespace vehicle_sil::ecu_main_detail {

/// The bytes of `text` in hexadecimal ("83 47"), for a text that cannot be shown as it is.
std::string hex_bytes(std::string_view text);

/// `text` in double quotes.
std::string in_quotes(std::string_view text);

/// `text` on one line: every run of line breaks (SIL Kit's messages have them) becomes a space.
std::string one_line(std::string_view text);

} // namespace vehicle_sil::ecu_main_detail
