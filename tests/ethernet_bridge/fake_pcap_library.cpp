// SPDX-License-Identifier: MIT
//
// TEST ONLY. The pcap library of the bridge executable built for the process tests
// (vehicle_sil_ethernet_bridge_on_fake_pcap): the file the environment variable
// VEHICLE_SIL_TEST_PCAP_LIBRARY names (the fake library of tests/eth_link/fake_pcap). Everything
// else of that executable is the installed one's code, with its factory of the links of the OS.
#include "pcap_library.hpp"

#include <cstdlib>
#include <stdexcept>

namespace vehicle_sil::ethernet_bridge {

std::string pcap_library()
{
    const char* const path = std::getenv("VEHICLE_SIL_TEST_PCAP_LIBRARY");
    if (path == nullptr || *path == '\0')
    {
        throw std::runtime_error("this is the bridge built for tests: set VEHICLE_SIL_TEST_PCAP_LIBRARY to the fake pcap library");
    }
    return path;
}

} // namespace vehicle_sil::ethernet_bridge
