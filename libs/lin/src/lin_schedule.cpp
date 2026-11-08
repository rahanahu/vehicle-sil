// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/lin_schedule.hpp"

#include <array>
#include <optional>
#include <stdexcept>
#include <string>

namespace vehicle_sil::lin {

void validate_schedule(const LinScheduleTable& table, SimDuration master_step)
{
    if (master_step <= SimDuration::zero())
    {
        throw std::invalid_argument("LIN schedule: the master step must be > 0");
    }
    if (table.empty())
    {
        throw std::invalid_argument("LIN schedule must not be empty");
    }
    std::array<const LinScheduleEntry*, kMaxLinId + 1> first{};
    for (const auto& entry : table)
    {
        const std::string what = "LIN schedule entry for id " + std::to_string(entry.id);
        if (!valid_lin_frame(entry.id, entry.data_length))
        {
            throw std::invalid_argument(what + ": id must be 0..63 and data_length 1..8");
        }
        if (entry.checksum_model != LinChecksumModel::Classic && entry.checksum_model != LinChecksumModel::Enhanced)
        {
            // SIL Kit skips the checksum check for LinChecksumModel::Unknown.
            throw std::invalid_argument(what + ": checksum_model must be Classic or Enhanced");
        }
        if (entry.id == kMasterRequestId || entry.id == kSlaveResponseId)
        {
            const auto publisher = entry.id == kMasterRequestId ? LinPublisher::Master : LinPublisher::Slave;
            if (entry.data_length != kDiagnosticFrameLength || entry.checksum_model != LinChecksumModel::Classic ||
                entry.publisher != publisher)
            {
                throw std::invalid_argument(what + ": a diagnostic frame has 8 bytes and the classic checksum, and the " +
                                            (publisher == LinPublisher::Master ? "master" : "a slave") + " publishes it");
            }
        }
        if (entry.slot <= SimDuration::zero() || !on_step_grid(entry.slot, master_step))
        {
            throw std::invalid_argument(what + ": slot must be a positive multiple of the master step");
        }
        const auto*& seen = first[entry.id];
        if (seen == nullptr)
        {
            seen = &entry;
        }
        else if (seen->data_length != entry.data_length || seen->checksum_model != entry.checksum_model ||
                 seen->publisher != entry.publisher)
        {
            throw std::invalid_argument(what + ": every entry for an id needs the same length, checksum and publisher");
        }
    }
}

} // namespace vehicle_sil::lin
