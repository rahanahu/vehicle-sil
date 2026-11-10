// SPDX-License-Identifier: MIT
//
// The parameter service of param_service.hpp between an engine ECU and a service tool, on a
// local virtual bus (vehicle_sil::LocalBench: one process, no SIL Kit): the tool reads and writes
// parameters and blocks, and meets the ECU's refusals. Exits with 1 if an answer is not the
// expected one.
#include "param_service.hpp"

#include "vehicle_sil/local/local_bench.hpp"

#include <cstdio>
#include <numeric>

int main()
{
    using namespace param_service;
    using vehicle_sil::j1939::Name;

    ParameterTable table;
    table.parameters = {
        {0x0100, Parameter{.value = 750, .writable = true, .min = 500, .max = 1200}}, // idle speed, rpm
        {0x0101, Parameter{.value = 2200, .writable = false}},                     // rated speed, rpm
    };
    table.blocks[1].resize(64);
    std::iota(table.blocks[1].begin(), table.blocks[1].end(), std::uint8_t{0}); // a calibration curve

    const Name engineName = Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_function(0);
    const Name toolName = Name{}.set_identity_number(2).set_manufacturer_code(0x7F0).set_function(129);

    vehicle_sil::LocalBench bus;
    auto& engine = bus.add_j1939("engine", 10ms).add_node({.address = 0x00, .name = engineName});
    auto& tool = bus.add_j1939("tool", 10ms).add_node({.address = 0xF9, .name = toolName});
    auto& server = engine.attach<ParamServer>(table);
    auto& client = tool.attach<ParamClient>(engineName);

    int unexpected = 0;
    const auto check = [&](const char* what, ParamStatus got, ParamStatus want) {
        std::printf("%-34s %s\n", what, got == want ? "as expected" : "UNEXPECTED");
        unexpected += got == want ? 0 : 1;
    };

    // The operations run one after the other; the first one opens the session.
    client.read(0x0100, [&](ParamStatus s, std::uint32_t v) {
        check("read idle speed", s, ParamStatus::Ok);
        std::printf("  idle speed = %u rpm\n", static_cast<unsigned>(v));
    });
    client.write(0x0100, 900, [&](ParamStatus s) { check("write idle speed 900 (Pending)", s, ParamStatus::Ok); });
    client.read(0x0100, [&](ParamStatus s, std::uint32_t v) {
        check("read it back", s, v == 900 ? ParamStatus::Ok : ParamStatus::Failed);
    });
    client.write(0x0101, 2500, [&](ParamStatus s) { check("write the rated speed", s, ParamStatus::ReadOnly); });
    client.write(0x0100, 5000, [&](ParamStatus s) { check("write idle speed 5000", s, ParamStatus::OutOfRange); });
    client.read_block(1, [&](ParamStatus s, std::span<const std::uint8_t> data) {
        check("read block 1 (transport protocol)", s, data.size() == 64 ? ParamStatus::Ok : ParamStatus::Failed);
    });
    const std::vector<std::uint8_t> curve(40, 0x5A);
    client.write_block(2, curve, [&](ParamStatus s) { check("write block 2 (transport protocol)", s, ParamStatus::Ok); });
    client.close([&](ParamStatus s) { check("close", s, ParamStatus::Ok); });

    bus.run_for(3s);

    std::printf("ECU: %llu session(s), %llu error answer(s); idle speed now %u rpm, block 2 has %zu bytes\n",
                static_cast<unsigned long long>(server.statistics().sessions),
                static_cast<unsigned long long>(server.statistics().errors),
                static_cast<unsigned>(table.parameters[0x0100].value), table.blocks[2].size());
    std::printf("tool: %llu command(s) sent\n", static_cast<unsigned long long>(client.statistics().sent));
    return unexpected == 0 && !server.in_session() && client.queued() == 0 ? 0 : 1;
}
