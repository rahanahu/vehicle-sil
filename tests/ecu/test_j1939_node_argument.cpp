// SPDX-License-Identifier: MIT
//
// The value of --j1939-node (vehicle_sil/ecu/j1939_node_argument.hpp): what the launcher writes is
// what an ECU executable reads, every wrong value has its own sentence, and the entries of one ECU
// are checked against each other.
#include "test_harness.hpp"

#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/ecu/j1939_node_argument.hpp"

#include <optional>
#include <string>
#include <vector>

using vehicle_sil::ecu_executable::format_j1939_node_argument;
using vehicle_sil::ecu_executable::J1939NodeOverride;
using vehicle_sil::ecu_executable::j1939_node_argument_missing;
using vehicle_sil::ecu_executable::j1939_node_arguments_error;
using vehicle_sil::ecu_executable::parse_j1939_node_argument;
using vehicle_sil::j1939::Address;
using vehicle_sil::j1939::NodeOverride;

namespace {

J1939NodeOverride entry(std::string network, std::optional<Address> from, std::optional<Address> address,
                        std::optional<std::uint32_t> identity_number = std::nullopt,
                        std::optional<std::uint8_t> function_instance = std::nullopt,
                        std::optional<std::uint8_t> ecu_instance = std::nullopt)
{
    J1939NodeOverride out;
    out.network = std::move(network);
    out.node.from = from;
    out.node.address = address;
    out.node.identity_number = identity_number;
    out.node.function_instance = function_instance;
    out.node.ecu_instance = ecu_instance;
    return out;
}

void check_text(const std::string& actual, const std::string& expected, const char* file, int line)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure(std::string(file) + ":" + std::to_string(line) + ": texts differ\n  actual:   " +
                                         actual + "\n  expected: " + expected);
    }
}

#define CHECK_TEXT(actual, expected) check_text((actual), (expected), __FILE__, __LINE__)

/// The error of parse_j1939_node_argument() for `text`; "(read)" if it was read.
std::string error_of(std::string_view text)
{
    const auto out = parse_j1939_node_argument(text);
    return out.entry ? "(read)" : out.error;
}

} // namespace

TEST_CASE(j1939_node_argument_names_are_fixed)
{
    CHECK(vehicle_sil::ecu_executable::kJ1939NodeArgument == "--j1939-node");
    CHECK(vehicle_sil::ecu_executable::kFeatureJ1939Node == "j1939-node");
    // The launcher gives it (j1939_nodes in the configuration), so a command of the configuration may not have it.
    bool given = false;
    for (const auto argument : vehicle_sil::ecu_executable::kLauncherArguments)
    {
        given = given || argument == vehicle_sil::ecu_executable::kJ1939NodeArgument;
    }
    CHECK(given);
}

TEST_CASE(j1939_node_argument_is_read_back_as_it_was_written)
{
    CHECK_TEXT(format_j1939_node_argument(entry("VehicleCAN", 0x00, 0x01, 2)), "VehicleCAN=from:0x00,address:0x01,identity_number:2");
    CHECK_TEXT(format_j1939_node_argument(entry("VehicleCAN", std::nullopt, 0x01)), "VehicleCAN=address:0x01");
    const std::vector<J1939NodeOverride> entries{
        entry("VehicleCAN", 0x00, 0x01, 2),
        entry("VehicleCAN", std::nullopt, 0x01),
        entry("VehicleCAN", std::nullopt, std::nullopt, 0),
        entry("VehicleCAN", 0xFD, std::nullopt, std::nullopt, 31),
        entry("VehicleCAN", 254, 0x80, 2097151, 31, 7),
        entry("VehicleCAN", 0x10, 254, std::nullopt, std::nullopt, 0),
        entry("Body CAN 2", 0xAB, 0xCD),         // a space in the name
        entry("N:1,a", 0x00, 0x01),              // the settings start after the first '='
        entry("\xE8\xBB\x8A\xE4\xB8\xA1", 0, 1), // UTF-8
    };
    for (const auto& e : entries)
    {
        const auto read = parse_j1939_node_argument(format_j1939_node_argument(e));
        CHECK(read.entry.has_value());
        CHECK(read.error.empty());
        CHECK(*read.entry == e);
    }
    // What a person writes: decimal, hexadecimal in either case, any order.
    CHECK(*parse_j1939_node_argument("VehicleCAN=from:0,address:1").entry == entry("VehicleCAN", 0x00, 0x01));
    CHECK(*parse_j1939_node_argument("VehicleCAN=identity_number:0X1f,address:0xfd,from:16").entry ==
          entry("VehicleCAN", 16, 0xFD, 31));
}

TEST_CASE(j1939_node_argument_that_is_wrong_says_what_to_write)
{
    CHECK_TEXT(j1939_node_argument_missing(),
               "--j1939-node needs <simulation's network name>=<setting>:<value>[,...] after it (e.g. --j1939-node "
               "VehicleCAN=from:0x00,address:0x01,identity_number:2)");
    CHECK_TEXT(error_of("VehicleCAN"),
               "--j1939-node needs <simulation's network name>=<setting>:<value>[,...], not \"VehicleCAN\" (e.g. "
               "--j1939-node VehicleCAN=from:0x00,address:0x01,identity_number:2: the node that the ECU's code puts at "
               "0x00 on VehicleCAN is at 0x01, with identity number 2)");
    CHECK_TEXT(error_of("=address:0x01"),
               "--j1939-node \"=address:0x01\" has an empty network name: it needs the simulation's name of a J1939 "
               "network before the '='");
    CHECK_TEXT(error_of("-x=address:0x01"),
               "--j1939-node \"-x=address:0x01\" has a network name that starts with '-': it needs the simulation's name "
               "of a J1939 network before the '='");
    const std::string emptySetting = " has an empty setting: write <setting>:<value> between the commas (settings: from, "
                                     "address, identity_number, function_instance, ecu_instance)";
    CHECK_TEXT(error_of("VehicleCAN="), "--j1939-node \"VehicleCAN=\"" + emptySetting);
    CHECK_TEXT(error_of("VehicleCAN=address:1,,from:0"), "--j1939-node \"VehicleCAN=address:1,,from:0\"" + emptySetting);
    CHECK_TEXT(error_of("VehicleCAN=address"),
               "--j1939-node \"VehicleCAN=address\" has the setting \"address\" without a value: write address:<value>");
    CHECK_TEXT(error_of("VehicleCAN=adress:0x01"),
               "--j1939-node \"VehicleCAN=adress:0x01\" has the unknown setting \"adress\": the settings are from, address, "
               "identity_number, function_instance, ecu_instance");
    CHECK_TEXT(error_of("VehicleCAN=address:0x01,address:0x02"),
               "--j1939-node \"VehicleCAN=address:0x01,address:0x02\" sets address twice: give each setting once");
    CHECK_TEXT(error_of("VehicleCAN=from:0x00"),
               "--j1939-node \"VehicleCAN=from:0x00\" changes nothing: give at least one of address, identity_number, "
               "function_instance, ecu_instance, or leave it out");
    CHECK_TEXT(error_of("VehicleCAN=address:x1"),
               "--j1939-node \"VehicleCAN=address:x1\" has address \"x1\", which is not a number: write it in decimal or as "
               "0x.. in hexadecimal");
    CHECK_TEXT(error_of("VehicleCAN=identity_number:2097152"),
               "--j1939-node \"VehicleCAN=identity_number:2097152\" has identity_number 2097152, which is out of its range "
               "0..2097151");
    CHECK_TEXT(error_of("VehicleCAN=from:255,address:1"),
               "--j1939-node \"VehicleCAN=from:255,address:1\" has from 255, which is out of its range 0..254");
    CHECK_TEXT(error_of("VehicleCAN=address:0xFF"),
               "--j1939-node \"VehicleCAN=address:0xFF\" has address 0xFF, which is out of its range 0..254");
    CHECK_TEXT(error_of("VehicleCAN=function_instance:32"),
               "--j1939-node \"VehicleCAN=function_instance:32\" has function_instance 32, which is out of its range 0..31");
    CHECK_TEXT(error_of("VehicleCAN=ecu_instance:8"),
               "--j1939-node \"VehicleCAN=ecu_instance:8\" has ecu_instance 8, which is out of its range 0..7");
    // A name in a code page (here Shift_JIS) is not UTF-8.
    CHECK_TEXT(error_of("\x83\x47"),
               "--j1939-node needs ASCII or UTF-8 text, not the bytes 83 47 (a name in another encoding, e.g. a Windows "
               "code page)");
    // The largest values are read.
    CHECK_TEXT(error_of("VehicleCAN=from:254,address:254,identity_number:2097151,function_instance:31,ecu_instance:7"), "(read)");
}

TEST_CASE(j1939_node_arguments_of_one_ecu_are_checked_against_each_other)
{
    const auto error = [](std::vector<J1939NodeOverride> entries) { return j1939_node_arguments_error(entries); };
    CHECK_TEXT(error({}), "");
    CHECK_TEXT(error({entry("VehicleCAN", std::nullopt, 0x01)}), "");
    // The same from on another network, and two nodes of one network, are fine.
    CHECK_TEXT(error({entry("VehicleCAN", 0x00, 0x01), entry("BodyCAN", 0x00, 0x01), entry("VehicleCAN", 0x21, 0x22),
                      entry("ToolCAN", std::nullopt, 0x05)}),
               "");
    CHECK_TEXT(error({entry("VehicleCAN", 0x00, 0x01), entry("BodyCAN", 0x00, 0x01), entry("VehicleCAN", 0x00, std::nullopt, 2)}),
               "--j1939-node VehicleCAN=from:0x00,address:0x01 and --j1939-node VehicleCAN=from:0x00,identity_number:2 "
               "change the same node (at 0x00 in the ECU's code): give its settings in one --j1939-node");
    const std::string withoutFrom =
        "--j1939-node VehicleCAN=address:0x01 has no from, so it is for the only node of the ECU on VehicleCAN, and "
        "--j1939-node VehicleCAN=from:0x21,address:0x22 is for the same network: give every --j1939-node of a network a from";
    CHECK_TEXT(error({entry("VehicleCAN", std::nullopt, 0x01), entry("VehicleCAN", 0x21, 0x22)}), withoutFrom);
    CHECK_TEXT(error({entry("VehicleCAN", 0x21, 0x22), entry("BodyCAN", 0x21, 0x22), entry("VehicleCAN", std::nullopt, 0x01)}),
               withoutFrom);
}
