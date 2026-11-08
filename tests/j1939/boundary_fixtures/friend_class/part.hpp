// SPDX-License-Identifier: MIT
// Fixture of the parts' boundary check (not compiled): befriending a class is refused.
#pragma once

class Part
{
    friend class Network;
};
