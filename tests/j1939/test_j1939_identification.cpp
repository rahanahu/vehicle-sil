// SPDX-License-Identifier: MIT
//
// The identification part of the J1939 stack: software, ECU and product identification
// and the diagnostic protocol identification on request; short answers padded to 8 bytes, long
// ones to all nodes (BAM) or to the requester (RTS/CTS), one after the other to one requester.
#include "j1939_parts_util.hpp"
#include "test_harness.hpp"
#include "vehicle_sil/j1939/identification.hpp"

#include <string>
#include <vector>

using namespace vehicle_sil::unit::j1939;
using vehicle_sil::j1939::EcuIdentification;
using vehicle_sil::j1939::Identification;
using vehicle_sil::j1939::Node;
using vehicle_sil::j1939::ProductIdentification;
using vehicle_sil::unit::throws;

namespace {

constexpr Address kUs = 0x20;
constexpr Address kTool = 0x90;
constexpr Address kOtherTool = 0x91;

J1939Frame req(Address from, Address to, std::uint32_t pgn) { return frame(6, kPgnRequest, from, to, request(pgn)); }

struct Ecu
{
    Ecu() : node(bench.add_node(kUs)), id(node.attach<Identification>())
    {
        bench.steps(2);
        (void)bench.take_sent();
    }
    Bench bench;
    Node& node;
    Identification& id;
};

const std::vector<std::string> kLongSoftware{"vehicle-sil 1.0", "native"};
const Bytes kLongSoftwareBytes = [] {
    Bytes out{0x02};
    for (const auto& b : text("vehicle-sil 1.0*native*"))
    {
        out.push_back(b);
    }
    return out;
}();

} // namespace

TEST_CASE(diagnostic_protocol_identification_on_request)
{
    Ecu ecu;
    const J1939Frame answer = frame(6, kPgnDiagnosticProtocolId, kUs, kGlobal, {0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF});
    ecu.bench.inject(req(kTool, kGlobal, kPgnDiagnosticProtocolId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {answer});
    ecu.bench.inject(req(kTool, kUs, kPgnDiagnosticProtocolId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {answer});
}

TEST_CASE(software_identification_without_fields_short_and_long)
{
    Ecu ecu;
    // Not set: "not available", answered (no NACK, nothing held back).
    ecu.bench.inject(req(kTool, kUs, kPgnSoftwareId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnSoftwareId, kUs, kGlobal, Bytes(8, 0xFF))});
    // Short: one frame padded to 8 bytes (the AgIsoStack++ version sent 5 bytes).
    const std::vector<std::string> one{"1.0"};
    ecu.id.set_software_identification(one);
    ecu.bench.inject(req(kTool, kGlobal, kPgnSoftwareId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnSoftwareId, kUs, kGlobal, padded({0x01, '1', '.', '0', '*'}))});
    // Long: BAM for a request to all nodes (the AgIsoStack++ version used RTS/CTS), RTS/CTS for
    // a destination-specific one.
    ecu.id.set_software_identification(kLongSoftware);
    ecu.bench.inject(req(kTool, kGlobal, kPgnSoftwareId));
    const auto messages = bam_messages(record(ecu.bench, 30), kUs, kPgnSoftwareId);
    CHECK_EQ(messages.size(), 1u);
    CHECK(messages[0] == kLongSoftwareBytes);
    ecu.bench.inject(req(kTool, kUs, kPgnSoftwareId));
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnSoftwareId) == kLongSoftwareBytes);
}

TEST_CASE(software_identification_requests_from_two_nodes_in_one_step_are_both_answered)
{
    Ecu ecu;
    ecu.id.set_software_identification(kLongSoftware);
    ecu.bench.inject(req(kTool, kUs, kPgnSoftwareId));
    ecu.bench.inject(req(kOtherTool, kUs, kPgnSoftwareId));
    ecu.bench.step();
    const auto sent = ecu.bench.take_sent();
    CHECK_J1939(sent, {rts(kUs, kTool, kLongSoftwareBytes.size(), kPgnSoftwareId),
                       rts(kUs, kOtherTool, kLongSoftwareBytes.size(), kPgnSoftwareId)});
}

TEST_CASE(two_identifications_requested_by_one_node_in_one_step_go_one_after_the_other)
{
    Ecu ecu;
    ecu.id.set_software_identification(kLongSoftware);
    ecu.id.set_ecu_identification(EcuIdentification{"PN-1", "SN-2", "", "", ""});
    ecu.bench.inject(req(kTool, kUs, kPgnSoftwareId));
    ecu.bench.inject(req(kTool, kUs, kPgnEcuId));
    // One connection per pair of nodes at a time: the ECU identification waits in the
    // response queue until the software identification is acknowledged.
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnSoftwareId) == kLongSoftwareBytes);
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnEcuId) == text("PN-1*SN-2****"));
}

TEST_CASE(ecu_and_product_identification_on_request)
{
    Ecu ecu;
    ecu.bench.inject(req(kTool, kGlobal, kPgnEcuId));
    ecu.bench.inject(req(kTool, kGlobal, kPgnProductId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnEcuId, kUs, kGlobal, padded(text("*****"))),
                                        frame(6, kPgnProductId, kUs, kGlobal, padded(text("***")))});
    ecu.id.set_ecu_identification(EcuIdentification{"PN-1", "SN-2", "Cab", "Body", "OSS"});
    ecu.id.set_product_identification(ProductIdentification{"1234567", "Open", "SIL"});
    ecu.bench.inject(req(kTool, kUs, kPgnEcuId));
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnEcuId) == text("PN-1*SN-2*Cab*Body*OSS*"));
    ecu.bench.inject(req(kTool, kUs, kPgnProductId));
    CHECK(receive_rts_cts(ecu.bench, kUs, kTool, kPgnProductId) == text("1234567*Open*SIL*"));
}

TEST_CASE(identification_fields_are_checked)
{
    Ecu ecu;
    CHECK(throws<std::invalid_argument>([&] { ecu.id.set_ecu_identification(EcuIdentification{"a*b", "", "", "", ""}); }));
    const std::vector<std::string> star{"x*"};
    CHECK(throws<std::invalid_argument>([&] { ecu.id.set_software_identification(star); }));
    const std::vector<std::string> tooLong{std::string(1785, 'x')};
    CHECK(throws<std::invalid_argument>([&] { ecu.id.set_software_identification(tooLong); }));
    const std::vector<std::string> tooMany(256, "x");
    CHECK(throws<std::invalid_argument>([&] { ecu.id.set_software_identification(tooMany); }));
    // The old value stays after a refused one.
    ecu.bench.inject(req(kTool, kGlobal, kPgnEcuId));
    ecu.bench.step();
    CHECK_J1939(ecu.bench.take_sent(), {frame(6, kPgnEcuId, kUs, kGlobal, padded(text("*****")))});
}
