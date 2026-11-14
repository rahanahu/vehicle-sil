// SPDX-License-Identifier: MIT
//
// Operating systems without a TAP link and a packet link (everything but Linux).
#include "vehicle_sil/eth_link/os_links.hpp"

namespace vehicle_sil::eth_link {

bool has_os_links() noexcept
{
    return false;
}

std::vector<InterfaceInfo> list_interfaces()
{
    return {};
}

std::unique_ptr<ExternalLink> make_tap_link(TapLinkOptions)
{
    throw LinkError(LinkErrorKind::Other, "a TAP link works on Linux only: run the bridge on a Linux computer.");
}

std::unique_ptr<ExternalLink> make_packet_link(PacketLinkOptions)
{
    throw LinkError(LinkErrorKind::Other, "a packet link works on Linux only: run the bridge on a Linux computer.");
}

} // namespace vehicle_sil::eth_link
