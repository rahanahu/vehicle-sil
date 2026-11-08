// SPDX-License-Identifier: MIT
// Fixture of the parts' boundary check (not compiled): a hidden friend function is allowed.
#pragma once

struct Sequence
{
    int value = 0;
    friend constexpr bool operator==(Sequence, Sequence) noexcept = default;
};
