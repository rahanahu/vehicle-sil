// SPDX-License-Identifier: MIT
//
// Helpers shared by the LIN master and slave implementations (not installed).
#pragma once

#include "vehicle_sil/lin/lin_types.hpp"

#include <silkit/services/lin/ILinController.hpp>

#include <exception>
#include <optional>

namespace vehicle_sil::lin::detail {

// Only used by a detailed (network simulator) LIN simulation; the trivial simulation ignores it.
inline constexpr SilKit::Services::Lin::LinBaudRate kBaudRate = 19200;

/// Removes a SIL Kit handler in a destructor. The participant may already be going away, in
/// which case nothing else can call the handler.
template <typename Remove>
void remove_handler(const std::optional<SilKit::Util::HandlerId>& id, Remove&& remove) noexcept
{
    if (!id)
    {
        return;
    }
    try
    {
        remove(*id);
    }
    catch (const std::exception&)
    {
    }
}

} // namespace vehicle_sil::lin::detail
