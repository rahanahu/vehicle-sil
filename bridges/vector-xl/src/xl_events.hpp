// SPDX-License-Identifier: MIT
//
// Internal: conversions between the backend's CAN messages (xl_backend.hpp) and SIL Kit CAN
// frames, implemented in xl_driver.cpp. Declared here so the tests can call them directly.
#pragma once

#include "xl_backend.hpp"

#include "vehicle_sil/vector_xl/can_bridge.hpp"

namespace vehicle_sil::vector_xl::detail {

/// Classifies one event from backend::receive().
enum class XlEventClass
{
    Frame,           ///< CAN frame from another node
    OwnTransmission, ///< transmit receipt or transmit request of our own frame
    ErrorFrame,
    Ignored,         ///< not a CAN message event (chip state, timer, sync pulse, ...)
};

struct ClassifiedXlEvent
{
    XlEventClass event_class = XlEventClass::Ignored;
    /// The library or the controller lost events before this one.
    bool overrun = false;
    /// For Frame and OwnTransmission. dataField points into the ReceivedEvent, so it is only
    /// valid while that event is.
    SilKit::Services::Can::CanFrame frame{};
};

[[nodiscard]] ClassifiedXlEvent classify_xl_event(const backend::ReceivedEvent& event) noexcept;

/// The message to transmit for a classic CAN frame that passed validate_classic_frame().
[[nodiscard]] backend::CanMessage to_transmit_message(const SilKit::Services::Can::CanFrame& frame) noexcept;

} // namespace vehicle_sil::vector_xl::detail
