// SPDX-License-Identifier: MIT
//
// A fixture for the boundary check: a vehicle-sil include that is in none of the project's
// include directories cannot be followed, so the check must reject it.
#pragma once

#include "vehicle_sil/nowhere/missing.hpp"
