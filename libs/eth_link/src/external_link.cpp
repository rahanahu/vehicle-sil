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

LinkError::LinkError(LinkErrorKind kind, const std::string& message) :
  std::runtime_error(message),
  kind_(kind)
{
}

} // namespace vehicle_sil::eth_link
