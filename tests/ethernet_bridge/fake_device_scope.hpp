// SPDX-License-Identifier: MIT
//
// TEST ONLY. The identity under which the bridge executables built for the process tests mark
// their fake devices (pcap_library.hpp, device_mark_identity()): "<device>@<scope>". The scope is
// this process ("pid<id>") unless the environment variable VEHICLE_SIL_FAKE_DEVICE_SCOPE names one,
// so that the tests that run at the same time on the same fake device do not find each other;
// only a test that wants two bridges to share a device gives both the same scope.
#pragma once

#include <cstdlib>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace vehicle_sil::ethernet_bridge::test {

inline std::string fake_device_identity(const std::string& device)
{
    if (const char* scope = std::getenv("VEHICLE_SIL_FAKE_DEVICE_SCOPE"); scope != nullptr && *scope != '\0')
    {
        return device + "@" + scope;
    }
#ifdef _WIN32
    return device + "@pid" + std::to_string(_getpid());
#else
    return device + "@pid" + std::to_string(::getpid());
#endif
}

} // namespace vehicle_sil::ethernet_bridge::test
