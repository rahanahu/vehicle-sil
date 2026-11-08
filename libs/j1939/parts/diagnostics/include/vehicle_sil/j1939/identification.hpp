// SPDX-License-Identifier: MIT
//
// A protocol part of the J1939 stack: the identification of one node (software, ECU and
// product identification, diagnostic protocol identification), answered on request. Built on
// the node's public API and the toolbox only.
#pragma once

#include "vehicle_sil/j1939/toolbox.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace vehicle_sil::j1939 {

inline constexpr Pgn kPgnSoftwareIdentification{0xFEDAu};
inline constexpr Pgn kPgnEcuIdentification{0xFDC5u};
inline constexpr Pgn kPgnProductIdentification{0xFC8Du};
inline constexpr Pgn kPgnDiagnosticProtocolIdentification{0xFD32u};

struct EcuIdentification
{
    std::string part_number;
    std::string serial_number;
    std::string location;
    std::string type;
    std::string manufacturer;
};

struct ProductIdentification
{
    std::string code;
    std::string brand;
    std::string model;
};

/// Answers Requests for the identification groups of one node with RequestContext::respond()
/// (up to 8 bytes one frame to all nodes, padded with 0xFF; longer to all nodes (BAM) or to the
/// requester (RTS/CTS) for a destination-specific request, waiting behind the node's transfer
/// that is still going).
/// - Software identification: the number of fields, then each field followed by '*'. Without
///   fields: 8 bytes 0xFF (not available).
/// - ECU identification: part number, serial number, location, type, manufacturer, each followed
///   by '*' (empty until set).
/// - Product identification: code, brand, model, each followed by '*' (empty until set).
/// - Diagnostic protocol identification: J1939-73 only (0x01, then 0xFF).
/// Answers are not suppressed by DM13. Create one per node, with node.attach<Identification>()
/// or owned by the application.
class Identification
{
public:
    explicit Identification(Node& node);
    Identification(const Identification&) = delete;
    Identification& operator=(const Identification&) = delete;

    /// The setters are allowed everywhere on the thread that steps the network (other threads:
    /// Host::post()); the next answer uses the new value. They allocate (do not call them in
    /// every step). std::invalid_argument for a field with '*', more than 255 software fields,
    /// or more than kMaxTransportLength bytes in all.
    void set_software_identification(std::span<const std::string> fields);
    void set_ecu_identification(const EcuIdentification& id);
    void set_product_identification(const ProductIdentification& id);

    /// Answers the node could not send (SendStatus other than Ok).
    [[nodiscard]] std::uint64_t send_failures() const noexcept { return send_failures_; }

private:
    void answer(RequestContext& request, const std::vector<std::uint8_t>& data);

    std::vector<std::uint8_t> software_;
    std::vector<std::uint8_t> ecu_;
    std::vector<std::uint8_t> product_;
    std::uint64_t send_failures_ = 0;
    Subscription software_request_;
    Subscription ecu_request_;
    Subscription product_request_;
    Subscription protocol_request_;
};

} // namespace vehicle_sil::j1939
