// SPDX-License-Identifier: MIT
// Fixture of the parts' boundary check (not compiled): befriending a type is refused.
#pragma once

class Part
{
    friend ::vehicle_sil::j1939::Node;
};
