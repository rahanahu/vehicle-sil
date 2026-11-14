// SPDX-License-Identifier: MIT
//
// CHECK_TEXT(actual, expected): like CHECK_EQ for text, printing both texts when they differ
// (the messages of the Ethernet bridge are checked word for word).
#pragma once

#include "test_harness.hpp"

#include <string>
#include <string_view>

namespace vehicle_sil::unit {

inline void check_text(std::string_view actual, std::string_view expected, const char* actualText, const char* file, int line)
{
    if (actual != expected)
    {
        throw Failure(std::string(file) + ":" + std::to_string(line) + ": CHECK_TEXT(" + actualText + ") failed:\n  actual:   " +
                      std::string(actual) + "\n  expected: " + std::string(expected));
    }
}

} // namespace vehicle_sil::unit

#define CHECK_TEXT(actual, expected) ::vehicle_sil::unit::check_text((actual), (expected), #actual, __FILE__, __LINE__)
