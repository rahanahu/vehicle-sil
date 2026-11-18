// SPDX-License-Identifier: MIT
#include "vehicle_sil/socketcan/error_frame.hpp"

#include <linux/can/error.h>

// Older kernel headers (for example those of the oldest Linux CI image) lack CAN_ERR_CNT. The value is fixed by the
// kernel ABI: "error counters in data[6] and data[7]".
#ifndef CAN_ERR_CNT
#define CAN_ERR_CNT 0x00000200U
#endif

namespace vehicle_sil::socketcan {

ErrorFrameState interpret_error_frame(std::uint32_t can_id, const std::array<std::uint8_t, 8>& data) noexcept
{
    using Level = can_bridge::ExternalBusState::Level;
    ErrorFrameState state;
    if ((can_id & CAN_ERR_CNT) != 0)
    {
        state.tx_errors = data[6];
        state.rx_errors = data[7];
    }
    if ((can_id & CAN_ERR_BUSOFF) != 0)
    {
        state.level = Level::BusOff;
    }
    else if ((can_id & CAN_ERR_RESTARTED) != 0)
    {
        state.level = Level::ErrorActive;
    }
    else if ((can_id & CAN_ERR_CRTL) != 0)
    {
        const std::uint8_t controller = data[1];
        if ((controller & (CAN_ERR_CRTL_RX_PASSIVE | CAN_ERR_CRTL_TX_PASSIVE)) != 0)
        {
            state.level = Level::ErrorPassive;
        }
        else if ((controller & (CAN_ERR_CRTL_RX_WARNING | CAN_ERR_CRTL_TX_WARNING)) != 0)
        {
            state.level = Level::ErrorWarning;
        }
        else if ((controller & CAN_ERR_CRTL_ACTIVE) != 0)
        {
            state.level = Level::ErrorActive;
        }
    }
    return state;
}

} // namespace vehicle_sil::socketcan
