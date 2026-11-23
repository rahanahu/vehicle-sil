// SPDX-License-Identifier: MIT
//
// Two ecu_main executables on one LIN network "DOOR", for the tests of vehicle_sil_launch serve (a launcher that runs
// one simulation with LIN again and again). Built twice from this file: SERVE_LIN_MASTER selects the master.
//
//   Master  the LIN master: every 20 ms the frame 0x10 (1 byte, the input "door.command", 0 to 200) and the header of
//           the frame 0x11 that the slave answers. Outputs: "door.answer" (the slave's last answer) and
//           "door.answers" (how many answers came).
//   Slave   a LIN slave that takes 0x10 (the output "door.command": the last one heard) and answers 0x11 with that
//           value plus one.
//
// Every value is a function of virtual time and of the writes, so two runs of one request give the same samples.
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <array>
#include <chrono>
#include <cstdint>

using namespace std::chrono_literals;
namespace lin = vehicle_sil::lin;
using vehicle_sil::SimTime;

namespace {

constexpr lin::LinId kCommand = 0x10;
constexpr lin::LinId kAnswer = 0x11;

#ifdef SERVE_LIN_MASTER
class Master
{
public:
    explicit Master(vehicle_sil::Ecu& ecu)
    {
        ecu.input("door.command", &command_, {.min = 0.0, .max = 200.0, .description = "sent to the slave in 0x10"});
        ecu.output("door.answer", &answer_, {.description = "the slave's last answer (0x11)"});
        ecu.output("door.answers", &answers_, {.description = "answers received"});
        auto& master = ecu.lin_master("DOOR", {{kCommand, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Master},
                                               {kAnswer, 1, lin::LinChecksumModel::Enhanced, 20ms, lin::LinPublisher::Slave}});
        master.on_result([this](const lin::LinScheduleResult& r) {
            if (r.id == kAnswer && r.status == lin::LinResultStatus::Ok)
            {
                answer_ = r.data[0];
                ++answers_;
            }
        });
        // Before the master's step: the header of this step carries the command.
        ecu.on_step([this, &master](SimTime) {
            master.set_frame_data(kCommand, std::array{static_cast<std::uint8_t>(command_)});
        });
    }

private:
    std::int64_t command_ = 0;
    std::int64_t answer_ = 0;
    std::int64_t answers_ = 0;
};
#else
class Slave
{
public:
    explicit Slave(vehicle_sil::Ecu& ecu)
    {
        ecu.output("door.command", &command_, {.description = "the last 0x10 heard"});
        auto& slave = ecu.lin_slave("DOOR", {.subscriptions = {kCommand}});
        slave.on_frame([this](const lin::LinReceivedFrame& f) { command_ = f.data[0]; });
        ecu.on_step([this, &slave](SimTime now) {
            slave.set_response(now, kAnswer, std::array{static_cast<std::uint8_t>(command_ + 1)});
        });
    }

private:
    std::int64_t command_ = 0;
};
#endif

} // namespace

int main(int argc, char** argv)
{
#ifdef SERVE_LIN_MASTER
    return vehicle_sil::ecu_main<Master>(argc, argv, {.name = "Master", .step = 10ms});
#else
    return vehicle_sil::ecu_main<Slave>(argc, argv, {.name = "Slave", .step = 5ms});
#endif
}
