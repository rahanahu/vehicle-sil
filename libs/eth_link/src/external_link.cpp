// SPDX-License-Identifier: MIT
#include "vehicle_sil/eth_link/external_link.hpp"

namespace vehicle_sil::eth_link {

std::string_view to_string(LinkState state) noexcept
{
    switch (state)
    {
    case LinkState::Up: return "link up";
    case LinkState::Down: return "link down";
    case LinkState::Unknown: break;
    }
    return "link state unknown";
}

std::string_view to_string(LinkErrorKind kind) noexcept
{
    switch (kind)
    {
    case LinkErrorKind::NoSuchInterface: return "no such interface";
    case LinkErrorKind::PermissionDenied: return "permission denied";
    case LinkErrorKind::LibraryMissing: return "library missing";
    case LinkErrorKind::NotEthernet: return "not Ethernet";
    case LinkErrorKind::Other: break;
    }
    return "other";
}

std::string_view to_string(Promiscuous mode) noexcept
{
    switch (mode)
    {
    case Promiscuous::Off: return "off";
    case Promiscuous::On: return "on";
    case Promiscuous::Auto: break;
    }
    return "auto";
}

std::string promiscuous_refused_warning(std::string_view name, std::string_view why)
{
    return "interface '" + std::string(name) + "' does not accept promiscuous mode (" + std::string(why) +
           "), so the bridge runs without it there: only frames to the interface's own address and to group addresses "
           "arrive, and frames to the devices of the SIL Kit side (they have addresses of their own) do not. If devices "
           "outside must reach them, use an adapter that accepts promiscuous mode (a wired one usually does).";
}

LinkError::LinkError(LinkErrorKind kind, const std::string& message) :
  std::runtime_error(message),
  kind_(kind)
{
}

} // namespace vehicle_sil::eth_link
