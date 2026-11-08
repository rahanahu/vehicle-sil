// SPDX-License-Identifier: MIT
#include "vehicle_sil/j1939/identification.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <stdexcept>
#include <string_view>

namespace vehicle_sil::j1939 {

namespace {

/// Each field followed by '*', padded with 0xFF to 8 bytes.
[[nodiscard]] std::vector<std::uint8_t> encode(std::vector<std::uint8_t> out, std::initializer_list<std::string_view> fields)
{
    for (const auto field : fields)
    {
        if (field.find('*') != std::string_view::npos)
        {
            throw std::invalid_argument("vehicle_sil::j1939::Identification: a field must not contain '*'");
        }
        out.insert(out.end(), field.begin(), field.end());
        out.push_back('*');
    }
    if (out.size() > kMaxTransportLength)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Identification: more than 1785 bytes");
    }
    out.resize(std::max<std::size_t>(out.size(), 8), 0xFF);
    return out;
}

} // namespace

Identification::Identification(Node& node)
  : software_(8, 0xFF), ecu_(encode({}, {"", "", "", "", ""})), product_(encode({}, {"", "", ""}))
{
    software_request_ = Subscription{node, node.on_request(kPgnSoftwareIdentification, [this](RequestContext& r) { answer(r, software_); })};
    ecu_request_ = Subscription{node, node.on_request(kPgnEcuIdentification, [this](RequestContext& r) { answer(r, ecu_); })};
    product_request_ = Subscription{node, node.on_request(kPgnProductIdentification, [this](RequestContext& r) { answer(r, product_); })};
    protocol_request_ = Subscription{node, node.on_request(kPgnDiagnosticProtocolIdentification, [this](RequestContext& r) {
        static constexpr std::array<std::uint8_t, 8> kJ1939_73{0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        if (r.respond(kJ1939_73) != SendStatus::Ok)
        {
            ++send_failures_;
        }
    })};
}

void Identification::answer(RequestContext& request, const std::vector<std::uint8_t>& data)
{
    if (request.respond(data) != SendStatus::Ok)
    {
        ++send_failures_;
    }
}

void Identification::set_software_identification(std::span<const std::string> fields)
{
    if (fields.empty())
    {
        software_.assign(8, 0xFF);
        return;
    }
    if (fields.size() > 255)
    {
        throw std::invalid_argument("vehicle_sil::j1939::Identification: more than 255 software fields");
    }
    std::vector<std::uint8_t> all{static_cast<std::uint8_t>(fields.size())};
    for (const auto& field : fields)
    {
        if (field.find('*') != std::string::npos)
        {
            throw std::invalid_argument("vehicle_sil::j1939::Identification: a field must not contain '*'");
        }
        all.insert(all.end(), field.begin(), field.end());
        all.push_back('*');
    }
    software_ = encode(std::move(all), {});
}

void Identification::set_ecu_identification(const EcuIdentification& id)
{
    ecu_ = encode({}, {id.part_number, id.serial_number, id.location, id.type, id.manufacturer});
}

void Identification::set_product_identification(const ProductIdentification& id)
{
    product_ = encode({}, {id.code, id.brand, id.model});
}

} // namespace vehicle_sil::j1939
