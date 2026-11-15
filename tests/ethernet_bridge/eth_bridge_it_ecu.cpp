// SPDX-License-Identifier: MIT
//
// TEST ONLY. An ecu_main executable for the scenario pcap-launcher of eth_bridge_it_launcher.cpp:
// on the Ethernet network ETH_EXT with the address 02:00:00:00:00:01, it sends one frame
// (EtherType 0x88B5) to 02:00:00:00:00:02 every 100 ms and counts the frames that come to its
// address. Behind the bridge, the device of the fake pcap library at 02:00:00:00:00:02 answers
// each frame (tests/eth_link/fake_pcap: fake0).
//
// Variables: sent (frames sent), answers (frames received at its address), both outputs.
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>

namespace {

constexpr vehicle_sil::eth::MacAddress kOwnMac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
constexpr vehicle_sil::eth::MacAddress kDeviceMac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};

class Pinger
{
public:
    explicit Pinger(vehicle_sil::Ecu& ecu) :
      channel_(ecu.ethernet("ETH_EXT", {.mac = kOwnMac, .accept = vehicle_sil::eth::Accept::Addressed}))
    {
        ecu.output("sent", &sent_, {.description = "frames sent to the device"});
        ecu.output("answers", &answers_, {.description = "frames received at this ECU's address"});
        channel_.on_frame([this](const vehicle_sil::eth::TimedFrame&) { ++answers_; });
        ecu.every(std::chrono::milliseconds(100), [this](vehicle_sil::SimTime) {
            std::array<std::uint8_t, 60> bytes{};
            std::copy(kDeviceMac.bytes.begin(), kDeviceMac.bytes.end(), bytes.begin());
            std::copy(kOwnMac.bytes.begin(), kOwnMac.bytes.end(), bytes.begin() + 6);
            bytes[12] = 0x88;
            bytes[13] = 0xB5;
            bytes[14] = static_cast<std::uint8_t>(sent_);
            if (channel_.send(bytes) == vehicle_sil::eth::SendStatus::Ok)
            {
                ++sent_;
            }
        });
    }

private:
    vehicle_sil::eth::Channel& channel_;
    int sent_ = 0;
    int answers_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    return vehicle_sil::ecu_main<Pinger>(argc, argv, {.name = "Pinger", .step = std::chrono::milliseconds(10)});
}
