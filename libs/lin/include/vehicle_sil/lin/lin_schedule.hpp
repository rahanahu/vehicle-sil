// SPDX-License-Identifier: MIT
//
// LIN schedule tables (vehicle_sil::lin).
#pragma once

#include "vehicle_sil/lin/lin_types.hpp"

#include <chrono>
#include <cstdint>
#include <vector>

namespace vehicle_sil::lin {

/// One slot of a LIN schedule table: the master sends the header for `id` at the start of the
/// slot, the publisher sends the response, and the next entry starts `slot` later.
struct LinScheduleEntry
{
    LinId id = 0;
    std::uint8_t data_length = 8; ///< 1..8 bytes
    LinChecksumModel checksum_model = LinChecksumModel::Enhanced;
    SimDuration slot{std::chrono::milliseconds(10)};
    LinPublisher publisher = LinPublisher::Slave;
};

/// A schedule table runs cyclically: after the last entry the master starts again at the first.
using LinScheduleTable = std::vector<LinScheduleEntry>;

/// Throws std::invalid_argument unless `table` is a valid schedule for a master stepping every
/// `master_step`: not empty, every entry a valid LIN frame with the classic or enhanced checksum
/// model and a slot that is a positive multiple of `master_step`, the diagnostic frames
/// (kMasterRequestId published by the master, kSlaveResponseId by a slave) with 8 bytes and the
/// classic checksum, and every identifier with one data length, checksum model and publisher
/// throughout the table.
void validate_schedule(const LinScheduleTable& table, SimDuration master_step);

} // namespace vehicle_sil::lin
