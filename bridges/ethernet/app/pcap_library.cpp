// SPDX-License-Identifier: MIT
#include "pcap_library.hpp"

namespace vehicle_sil::ethernet_bridge {

std::string pcap_library()
{
    return {};
}

std::string device_mark_identity(const std::string& device)
{
    return device;
}

std::optional<Backend> default_backend() noexcept
{
#if defined(_WIN32)
    return Backend::Npcap;
#elif defined(__linux__)
    return Backend::Packet;
#else
    return std::nullopt;
#endif
}

} // namespace vehicle_sil::ethernet_bridge
