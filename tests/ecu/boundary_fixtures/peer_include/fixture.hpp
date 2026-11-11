// SPDX-License-Identifier: MIT
//
// A fixture for the boundary check: a header that reaches SIL Kit through a vehicle-sil header
// (Peer includes SIL Kit's CAN port and the participant runtime). The check must reject it.
#pragma once

#include "vehicle_sil/peer/peer.hpp"
