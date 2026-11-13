// SPDX-License-Identifier: MIT
//
// The kinds of network an ECU can be on, on the local virtual buses and on SIL Kit: a name is one
// kind for every ECU, and the refusal says which kind it is and how it was made.
#include "test_harness.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;

const lin::LinScheduleTable kTable{{0x10, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Master}};

/// The message of the std::invalid_argument that `fn` throws ("(nothing thrown)" if none).
std::string refusal(const std::function<void()>& fn)
{
    try
    {
        fn();
    }
    catch (const std::invalid_argument& e)
    {
        return e.what();
    }
    return "(nothing thrown)";
}

void check_text(const std::string& actual, const std::string& expected)
{
    if (actual != expected)
    {
        throw unit::Failure("expected\n  " + expected + "\nbut got\n  " + actual);
    }
}

/// Puts `ecu` on the network `name` with `use`. A new NetworkUse does not build here (-Wswitch)
/// until it has its call: the tests below then cover it on both runtimes.
void put_on(Ecu& ecu, NetworkUse use, const std::string& name)
{
    switch (use)
    {
    case NetworkUse::Raw:
        static_cast<void>(ecu.can(name));
        return;
    case NetworkUse::J1939:
        ecu.j1939(name);
        return;
    case NetworkUse::LinMaster:
        static_cast<void>(ecu.lin_master(name, kTable));
        return;
    case NetworkUse::LinSlave:
        static_cast<void>(ecu.lin_slave(name));
        return;
    }
    throw unit::Failure("put_on(): a NetworkUse outside the enumerators");
}

std::optional<SilKitOptions> on(bool silkit)
{
    return silkit ? std::optional<SilKitOptions>(SilKitOptions{}) : std::nullopt;
}

} // namespace

TEST_CASE(every_use_of_a_network_is_described_on_both_runtimes)
{
    // Each kind of ECU implementation (Peer on SIL Kit, the bench's ECU locally) keeps each use
    // apart: describe() must report every one, in the order of the calls, with its kind.
    for (const bool silkit : {false, true})
    {
        Simulation sim(on(silkit));
        Ecu& ecu = sim.add_ecu("A", 5ms);
        for (const NetworkUse use : kNetworkUses)
        {
            put_on(ecu, use, std::string("N_") + to_string(use));
        }
        const EcuDescription d = ecu.describe();
        CHECK_EQ(d.networks.size(), kNetworkUses.size());
        for (std::size_t i = 0; i < d.networks.size() && i < kNetworkUses.size(); ++i)
        {
            CHECK(d.networks[i].name == std::string("N_") + to_string(kNetworkUses[i]));
            CHECK(d.networks[i].use == kNetworkUses[i]);
            CHECK(d.networks[i].kind() == kind_of(kNetworkUses[i]));
        }
    }
}

TEST_CASE(every_use_refuses_a_name_that_is_a_network_of_another_kind)
{
    // For each pair of uses of different kinds: the second is refused, in the same ECU and in
    // another ECU of the simulation (the simulation's check on SIL Kit, the bench's locally).
    for (const bool silkit : {false, true})
    {
        for (const NetworkUse first : kNetworkUses)
        {
            for (const NetworkUse second : kNetworkUses)
            {
                if (kind_of(first) == kind_of(second))
                {
                    continue;
                }
                const std::string what = std::string(silkit ? "SIL Kit" : "local") + ", " + to_string(first) +
                                         " then " + to_string(second);
                Simulation sim(on(silkit));
                Ecu& a = sim.add_ecu("A", 5ms);
                Ecu& b = sim.add_ecu("B", 5ms);
                put_on(a, first, "N");
                if (refusal([&] { put_on(a, second, "N"); }) == "(nothing thrown)")
                {
                    throw unit::Failure(what + ": the same ECU took both");
                }
                if (refusal([&] { put_on(b, second, "N"); }) == "(nothing thrown)")
                {
                    throw unit::Failure(what + ": another ECU took the second");
                }
            }
        }
    }
}

TEST_CASE(a_network_of_another_ecu_of_the_other_kind_is_refused_with_its_kind)
{
    // On SIL Kit, the ECUs of this process: the simulation's check names the other ECU.
    {
        Simulation sim(SilKitOptions{});
        Ecu& a = sim.add_ecu("A", 5ms);
        Ecu& b = sim.add_ecu("B", 5ms);
        a.j1939("CAN1");
        static_cast<void>(a.lin_slave("LIN1"));
        check_text(refusal([&] { static_cast<void>(b.lin_master("CAN1", kTable)); }),
                   "vehicle_sil::Simulation: lin_master(\"CAN1\") of the ECU \"B\": \"CAN1\" is a CAN network of the ECU "
                   "\"A\" (made by j1939() or can()); a LIN network needs a name of its own");
        check_text(refusal([&] { b.j1939("LIN1"); }),
                   "vehicle_sil::Simulation: j1939(\"LIN1\") of the ECU \"B\": \"LIN1\" is a LIN network of the ECU \"A\" "
                   "(made by lin_master() or lin_slave()); a CAN network needs a name of its own");
    }
    // Locally, the buses of the bench.
    {
        Simulation sim;
        Ecu& a = sim.add_ecu("A", 5ms);
        Ecu& b = sim.add_ecu("B", 5ms);
        a.j1939("CAN1");
        static_cast<void>(a.lin_slave("LIN1"));
        check_text(refusal([&] { static_cast<void>(b.lin_master("CAN1", kTable)); }),
                   "vehicle_sil::LocalBench: lin_master(\"CAN1\") of the ECU \"B\": \"CAN1\" is a CAN bus (made by "
                   "j1939() or can()); a LIN bus needs a name of its own");
        check_text(refusal([&] { b.j1939("LIN1"); }),
                   "vehicle_sil::LocalBench: j1939(\"LIN1\") of the ECU \"B\": \"LIN1\" is a LIN bus (made by "
                   "lin_master() or lin_slave()); a CAN bus needs a name of its own");
    }
}
