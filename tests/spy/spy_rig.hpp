// SPDX-License-Identifier: MIT
//
// What the tests of vehicle_sil::spy share: an ECU on a LocalBench with a J1939 network, frames
// injected on its bus, and its variables read as a scenario would.
#pragma once

#include "j1939_bench.hpp"
#include "j1939_transport_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/agent.hpp"
#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/local/local_bench.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace vehicle_sil::unit::spy {

using namespace vehicle_sil::unit::j1939;

inline constexpr const char* kBus = "J1939_CAN";

/// The ECU "Spy" (step 10 ms by default) on the bus kBus of a LocalBench.
struct Rig
{
    LocalBench bench;
    Ecu& ecu;
    vehicle_sil::j1939::Network& network;

    explicit Rig(vehicle_sil::j1939::NetworkOptions options = {}, SimDuration step = std::chrono::milliseconds(10)) :
        ecu(bench.add_ecu("Spy", step)), network(ecu.j1939(kBus, options))
    {
    }

    void inject(SimTime time, const J1939Frame& frame)
    {
        CHECK(bench.inject(kBus, time, to_can(frame)) == vehicle_sil::can::SendResult::Accepted);
    }
};

/// The latest sampled value of the variable `name` of `ecu`.
inline control::Value value_of(Ecu& ecu, std::string_view name)
{
    control::Agent& agent = ecu.agent();
    const auto& schema = agent.schema();
    const control::Snapshot snapshot = agent.latest();
    for (std::size_t i = 0; i < schema.size(); ++i)
    {
        if (schema[i].name == name)
        {
            return snapshot.values.at(i);
        }
    }
    throw Failure("no variable " + std::string(name));
}

inline std::int64_t integer_of(Ecu& ecu, std::string_view name) { return std::get<std::int64_t>(value_of(ecu, name)); }

/// The declaration of the variable `name` of `ecu`.
inline control::VariableInfo info_of(Ecu& ecu, std::string_view name)
{
    for (const auto& info : ecu.agent().schema())
    {
        if (info.name == name)
        {
            return info;
        }
    }
    throw Failure("no variable " + std::string(name));
}

inline bool has_variable(Ecu& ecu, std::string_view name)
{
    for (const auto& info : ecu.describe().variables)
    {
        if (info.name == name)
        {
            return true;
        }
    }
    return false;
}

/// The message of the E that `f` throws; a Failure if it throws none.
template <typename E, typename F>
std::string message_of(F&& f)
{
    try
    {
        f();
    }
    catch (const E& e)
    {
        return e.what();
    }
    throw Failure("no exception");
}

/// CHECK(message == expected) that shows both on a failure.
inline void check_message(const std::string& message, const std::string& expected, const char* file, int line)
{
    if (message != expected)
    {
        throw Failure(std::string(file) + ":" + std::to_string(line) + ": message\n  " + message + "\nexpected\n  " +
                      expected);
    }
}
#define CHECK_MESSAGE(message, expected) ::vehicle_sil::unit::spy::check_message((message), (expected), __FILE__, __LINE__)

} // namespace vehicle_sil::unit::spy
