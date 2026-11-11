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
        throw std::invalid_argument("vehicle_sil::lin: the master step of a schedule must be > 0 (pass the step of the ECU that runs the master)");
    }
    if (table.empty())
    {
        throw std::invalid_argument("vehicle_sil::lin: the schedule is empty; give it at least one entry");
    }
    std::array<const LinScheduleEntry*, kMaxLinId + 1> first{};
    for (const auto& entry : table)
    {
        const std::string what = "vehicle_sil::lin: schedule entry for id " + std::to_string(entry.id);
        if (!valid_lin_frame(entry.id, entry.data_length))
        {
            throw std::invalid_argument(what + " with data_length " + std::to_string(entry.data_length) +
                                        ": the id must be 0..63 and the data_length 1..8");
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
            throw std::invalid_argument(what + ": the slot (" + std::to_string(entry.slot.count()) +
                                        " ns) must be a positive multiple of the master step (" +
                                        std::to_string(master_step.count()) + " ns)");
        }
        const auto*& seen = first[entry.id];
        if (seen == nullptr)
        {
            seen = &entry;
        }
        else if (seen->data_length != entry.data_length || seen->checksum_model != entry.checksum_model ||
                 seen->publisher != entry.publisher)
        {
            throw std::invalid_argument(what +
                                        ": every entry for an id needs the same data_length, checksum_model and publisher");
        }
    }
}

} // namespace vehicle_sil::lin
