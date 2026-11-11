// SPDX-License-Identifier: MIT
//
// to_json(EcuDescription): the whole text of an empty and of a full description (keys, their
// order, layout, numbers, exact 64-bit enum values, the NAME as a string, null for what is absent
// or not finite), the escaping of strings, invalid UTF-8, enumeration values outside their
// enumerators, the names of the kinds and uses, comparisons, the version.
#include "test_harness.hpp"

#include "vehicle_sil/ecu/describe.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

using namespace std::chrono_literals;
using vehicle_sil::EcuDescription;
using vehicle_sil::NetworkKind;
using vehicle_sil::NetworkUse;
using vehicle_sil::to_json;
namespace control = vehicle_sil::control;
namespace j1939 = vehicle_sil::j1939;

namespace {

const std::string kVersion = vehicle_sil::version();

/// Throws a Failure that shows both texts.
void check_text(const std::string& actual, const std::string& expected)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure("text differs.\n--- actual ---\n" + actual + "--- expected ---\n" + expected);
    }
}

/// One variable with only a name, of `type`.
control::VariableInfo variable(std::string name, control::ValueType type = control::ValueType::Real)
{
    control::VariableInfo v;
    v.name = std::move(name);
    v.type = type;
    return v;
}

} // namespace

TEST_CASE(empty_description_has_every_key)
{
    check_text(to_json(EcuDescription{.name = "Empty", .step = 1ms}),
               "{\n"
               "  \"vehicle_sil_ecu\": 1,\n"
               "  \"vehicle_sil\": \"" + kVersion + "\",\n"
               "  \"name\": \"Empty\",\n"
               "  \"step_ns\": 1000000,\n"
               "  \"networks\": [],\n"
               "  \"variables\": []\n"
               "}\n");
}

TEST_CASE(real_time_step_is_null)
{
    check_text(to_json(EcuDescription{.name = "Bridge"}),
               "{\n"
               "  \"vehicle_sil_ecu\": 1,\n"
               "  \"vehicle_sil\": \"" + kVersion + "\",\n"
               "  \"name\": \"Bridge\",\n"
               "  \"step_ns\": null,\n"
               "  \"networks\": [],\n"
               "  \"variables\": []\n"
               "}\n");
}

TEST_CASE(full_description)
{
    EcuDescription d;
    d.name = "Engine";
    d.step = 2500us;
    const auto name = j1939::Name{}.set_identity_number(1).set_manufacturer_code(0x7F0).set_arbitrary_address_capable(true);
    d.networks.push_back({"J1939_CAN", NetworkUse::J1939, {{0x00, std::nullopt}, {j1939::kNullAddress, name}}});
    d.networks.push_back({"CAN2", NetworkUse::Raw, {}});
    d.networks.push_back({"LIN1", NetworkUse::LinMaster, {}});
    d.networks.push_back({"LIN2", NetworkUse::LinSlave, {}});
    control::VariableInfo throttle = variable("engine.throttle");
    throttle.unit = "%";
    throttle.min = 0.0;
    throttle.max = 100.0;
    throttle.step = 0.1;
    throttle.description = "pedal";
    d.variables.push_back(throttle);
    control::VariableInfo mode = variable("engine.mode", control::ValueType::Enum);
    mode.causality = control::Causality::Parameter;
    mode.min = -1.0;
    mode.max = 2.0;
    mode.items = {{"off", -1}, {"idle", 0}, {"run", 2}, {"far", (std::int64_t{1} << 60) + 1},
                  {"lowest", std::numeric_limits<std::int64_t>::min()}};
    d.variables.push_back(mode);
    control::VariableInfo running = variable("engine.running", control::ValueType::Bool);
    running.causality = control::Causality::Output;
    d.variables.push_back(running);
    control::VariableInfo count = variable("engine.count", control::ValueType::Integer);
    count.causality = control::Causality::Output;
    count.min = -9007199254740993.0; // not exact as a double: written as the double it is
    count.max = 1e300;
    d.variables.push_back(count);

    check_text(to_json(d),
               R"({
  "vehicle_sil_ecu": 1,
  "vehicle_sil": ")" + kVersion + R"(",
  "name": "Engine",
  "step_ns": 2500000,
  "networks": [
    {
      "name": "J1939_CAN",
      "kind": "can",
      "use": "j1939",
      "nodes": [
        {
          "address": 0,
          "j1939_name": null,
          "arbitrary_address_capable": false
        },
        {
          "address": 254,
          "j1939_name": "0x80000000FE000001",
          "arbitrary_address_capable": true
        }
      ]
    },
    {
      "name": "CAN2",
      "kind": "can",
      "use": "raw",
      "nodes": []
    },
    {
      "name": "LIN1",
      "kind": "lin",
      "use": "lin_master",
      "nodes": []
    },
    {
      "name": "LIN2",
      "kind": "lin",
      "use": "lin_slave",
      "nodes": []
    }
  ],
  "variables": [
    {
      "name": "engine.throttle",
      "type": "real",
      "causality": "input",
      "unit": "%",
      "min": 0,
      "max": 100,
      "step": 0.1,
      "description": "pedal",
      "items": []
    },
    {
      "name": "engine.mode",
      "type": "enum",
      "causality": "parameter",
      "unit": "",
      "min": -1,
      "max": 2,
      "step": null,
      "description": "",
      "items": [
        {
          "name": "off",
          "value": -1
        },
        {
          "name": "idle",
          "value": 0
        },
        {
          "name": "run",
          "value": 2
        },
        {
          "name": "far",
          "value": 1152921504606846977
        },
        {
          "name": "lowest",
          "value": -9223372036854775808
        }
      ]
    },
    {
      "name": "engine.running",
      "type": "bool",
      "causality": "output",
      "unit": "",
      "min": null,
      "max": null,
      "step": null,
      "description": "",
      "items": []
    },
    {
      "name": "engine.count",
      "type": "integer",
      "causality": "output",
      "unit": "",
      "min": -9007199254740992,
      "max": 1e+300,
      "step": null,
      "description": "",
      "items": []
    }
  ]
}
)");
}

TEST_CASE(numbers_read_back_to_the_same_double)
{
    // The shortest text of each value, which reads back to the same double.
    const double values[] = {0.1, 1.0 / 3.0, -2.5e-8, 123456789.125, std::numeric_limits<double>::max(),
                             std::numeric_limits<double>::denorm_min(), -0.0};
    for (const double value : values)
    {
        EcuDescription d{.name = "N", .step = 1ms};
        control::VariableInfo v = variable("v");
        v.min = value;
        d.variables.push_back(v);
        const std::string json = to_json(d);
        const std::string key = "\"min\": ";
        const auto at = json.find(key) + key.size();
        const std::string text = json.substr(at, json.find(',', at) - at);
        // std::from_chars: std::stod refuses subnormal values (ERANGE).
        double back = 1.0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), back);
        CHECK(error == std::errc{} && end == text.data() + text.size());
        CHECK(back == value);
        CHECK(std::signbit(back) == std::signbit(value));
    }
}

TEST_CASE(not_finite_numbers_are_null)
{
    EcuDescription d{.name = "N", .step = 1ms};
    control::VariableInfo v = variable("v");
    v.min = -std::numeric_limits<double>::infinity();
    v.max = std::numeric_limits<double>::infinity();
    v.step = std::numeric_limits<double>::quiet_NaN();
    d.variables.push_back(v);
    const std::string json = to_json(d);
    CHECK(json.find("      \"min\": null,\n      \"max\": null,\n      \"step\": null,\n") != std::string::npos);
}

TEST_CASE(strings_are_escaped)
{
    // Quote, backslash, the short escapes, other control characters as \u00XX, and UTF-8 and
    // DEL (0x7F) as they are.
    const std::string name = std::string("a\"b\\c\b\f\n\r\t") + '\x01' + '\x1f' + "\x7f" + "\xE6\x97\xA5" + '/';
    const std::string json = to_json(EcuDescription{.name = name, .step = 1ms});
    check_text(json.substr(json.find("\"name\""), json.find(",\n  \"step_ns\"") - json.find("\"name\"")),
               "\"name\": \"a\\\"b\\\\c\\b\\f\\n\\r\\t\\u0001\\u001f\x7f\xE6\x97\xA5/\"");
}

TEST_CASE(same_description_same_bytes)
{
    EcuDescription d{.name = "E", .step = 10ms};
    d.networks.push_back({"CAN", NetworkUse::J1939, {{0x10, j1939::Name{}.set_identity_number(42).set_manufacturer_code(0x7F0)}}});
    d.variables.push_back(variable("x"));
    const EcuDescription copy = d;
    CHECK(to_json(d) == to_json(copy));
    CHECK(to_json(d) == to_json(d));
}

TEST_CASE(descriptions_compare_equal)
{
    EcuDescription d{.name = "E", .step = 10ms};
    d.networks.push_back({"CAN", NetworkUse::J1939, {{0x10, std::nullopt}}});
    d.variables.push_back(variable("x"));
    EcuDescription other = d;
    CHECK(d == other);
    other.variables[0].unit = "V";
    CHECK(!(d == other));
    other = d;
    other.networks[0].nodes[0].address = 0x11;
    CHECK(!(d == other));
    other = d;
    other.step = std::nullopt;
    CHECK(!(d == other));
}

TEST_CASE(invalid_utf8_becomes_replacement_characters)
{
    // Each byte that starts no valid sequence becomes U+FFFD (EF BF BD); valid sequences stay.
    struct Case
    {
        std::string in;
        std::string out;
    };
    const std::string r = "\xEF\xBF\xBD";
    const std::vector<Case> cases{
        {"a\xFF" "b", "a" + r + "b"},                        // a byte that never starts a sequence
        {"\xE6\x97", r + r},                                 // a sequence cut short at the end
        {"\xE6\x97" "x", r + r + "x"},                         // ... and in the middle
        {"\xC0\xAF", r + r},                                 // an overlong form of '/'
        {"\xED\xA0\x80", r + r + r},                         // a surrogate
        {"\xF4\x90\x80\x80", r + r + r + r},                  // above U+10FFFF
        {"\x83\x5C", r + "\\\\"},                            // CP932: the trail byte 0x5C is a backslash
        {"\xE6\x97\xA5\xF0\x9F\x9A\x97", "\xE6\x97\xA5\xF0\x9F\x9A\x97"}, // valid: as they are
    };
    for (const auto& c : cases)
    {
        const std::string json = to_json(EcuDescription{.name = c.in, .step = 1ms});
        check_text(json.substr(json.find("\"name\": "), json.find(",\n  \"step_ns\"") - json.find("\"name\": ")),
                   "\"name\": \"" + c.out + "\"");
        CHECK(vehicle_sil::detail::is_utf8(c.out));
    }
    CHECK(vehicle_sil::detail::is_utf8("plain ASCII"));
    CHECK(vehicle_sil::detail::is_utf8("\xE6\x97\xA5\xE6\x9C\xAC"));
    CHECK(!vehicle_sil::detail::is_utf8("\xE6\x97"));
    CHECK(!vehicle_sil::detail::is_utf8("\x83G\x83\x93"));
}

TEST_CASE(enumeration_values_outside_the_enumerators_are_refused)
{
    EcuDescription d{.name = "E", .step = 1ms};
    d.networks.push_back({"CAN", static_cast<NetworkUse>(42), {}});
    CHECK(vehicle_sil::unit::throws_with<std::logic_error>([&] { static_cast<void>(to_json(d)); }, "NetworkUse 42"));
    CHECK(vehicle_sil::unit::throws<std::logic_error>([] { static_cast<void>(vehicle_sil::to_string(static_cast<NetworkKind>(7))); }));
    EcuDescription v{.name = "E", .step = 1ms};
    v.variables.push_back(variable("x", static_cast<control::ValueType>(9)));
    CHECK(vehicle_sil::unit::throws_with<std::logic_error>([&] { static_cast<void>(to_json(v)); }, "control::ValueType 9"));
    v.variables[0] = variable("x");
    v.variables[0].causality = static_cast<control::Causality>(5);
    CHECK(vehicle_sil::unit::throws_with<std::logic_error>([&] { static_cast<void>(to_json(v)); }, "control::Causality 5"));
}

TEST_CASE(version_is_the_projects)
{
    CHECK(std::string(vehicle_sil::version()) == VEHICLE_SIL_TEST_PROJECT_VERSION);
    CHECK(!std::string(vehicle_sil::version()).empty());
}

TEST_CASE(kind_and_use_names)
{
    CHECK(std::string(vehicle_sil::to_string(NetworkKind::Can)) == "can");
    CHECK(std::string(vehicle_sil::to_string(NetworkKind::Lin)) == "lin");
    CHECK(std::string(vehicle_sil::to_string(NetworkUse::Raw)) == "raw");
    CHECK(std::string(vehicle_sil::to_string(NetworkUse::J1939)) == "j1939");
    CHECK(std::string(vehicle_sil::to_string(NetworkUse::LinMaster)) == "lin_master");
    CHECK(std::string(vehicle_sil::to_string(NetworkUse::LinSlave)) == "lin_slave");
    CHECK(vehicle_sil::kind_of(NetworkUse::Raw) == NetworkKind::Can);
    CHECK(vehicle_sil::kind_of(NetworkUse::J1939) == NetworkKind::Can);
    CHECK(vehicle_sil::kind_of(NetworkUse::LinMaster) == NetworkKind::Lin);
    CHECK(vehicle_sil::kind_of(NetworkUse::LinSlave) == NetworkKind::Lin);
}
