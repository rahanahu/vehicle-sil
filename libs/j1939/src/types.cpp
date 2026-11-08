// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/types.hpp"

#include <stdexcept>

namespace vehicle_sil::j1939 {

std::uint32_t Identifier::encode() const
{
    if (!pgn.is_valid())
    {
        throw std::invalid_argument("vehicle_sil::j1939::Identifier: invalid PGN");
    }
    if (priority > kMaxPriority)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Identifier: priority above 7");
    }
    std::uint32_t pgnField = pgn.value();
    if (pgn.is_pdu1())
    {
        pgnField |= destination;
    }
    else if (destination != kGlobalAddress)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Identifier: a PDU2 group goes to the global address only");
    }
    return (static_cast<std::uint32_t>(priority) << 26) | (pgnField << 8) | source;
}

Identifier Identifier::decode(std::uint32_t id) noexcept
{
    Identifier out;
    out.priority = static_cast<Priority>((id >> 26) & 0x7u);
    out.source = static_cast<Address>(id & 0xFFu);
    const std::uint32_t pgnField = (id >> 8) & 0x3FFFFu;
    const Pgn raw{pgnField};
    if (raw.is_pdu1())
    {
        out.pgn = Pgn{pgnField & 0x3FF00u};
        out.destination = raw.pdu_specific();
    }
    else
    {
        out.pgn = raw;
        out.destination = kGlobalAddress;
    }
    return out;
}

std::optional<Identifier> Identifier::decode(const can::Frame& frame) noexcept
{
    if (frame.format != can::IdFormat::Extended)
    {
        return std::nullopt;
    }
    return decode(frame.id);
}

} // namespace vehicle_sil::j1939
