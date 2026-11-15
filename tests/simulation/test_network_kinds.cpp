// SPDX-License-Identifier: MIT
//
// The kinds of network an ECU can be on, on the local virtual buses and on SIL Kit: a name is one
// kind for every ECU, and the refusal says which kind it is and how it was made. And the other
// names of the networks (SilKitOptions::network_names): every use of a network is on the SIL Kit
// network of the simulation's name.
#include "test_harness.hpp"

#include "vehicle_sil/simulation/simulation.hpp"

#include <array>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

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
    case NetworkUse::Ethernet:
        static_cast<void>(ecu.ethernet(name));
        return;
    }
    throw unit::Failure("put_on(): a NetworkUse outside the enumerators");
}

std::optional<SilKitOptions> on(bool silkit)
{
    return silkit ? std::optional<SilKitOptions>(SilKitOptions{}) : std::nullopt;
}

constexpr lin::LinId kFromMaster = 0x10;
constexpr lin::LinId kFromSlave = 0x20;
const lin::LinScheduleTable kBothWays{{kFromMaster, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Master},
                                      {kFromSlave, 1, lin::LinChecksumModel::Enhanced, 10ms, lin::LinPublisher::Slave}};
constexpr eth::MacAddress kSpeakerMac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};

/// Puts `ecu` on the network `name` with `use` and makes it send there all the time. A new
/// NetworkUse does not build here either: the test of the other names then covers it.
void speak_on(Ecu& ecu, NetworkUse use, const std::string& name)
{
    switch (use)
    {
    case NetworkUse::Raw:
    {
        auto& raw = ecu.can(name);
        ecu.every(10ms, [&raw](SimTime) {
            can::Frame f;
            f.id = 0x123;
            f.format = can::IdFormat::Standard;
            f.length = 1;
            static_cast<void>(raw.send(f));
        });
        return;
    }
    case NetworkUse::J1939:
    {
        auto& node = ecu.j1939(name).add_node({.address = 0x10});
        ecu.every(10ms, [&node](SimTime) {
            const std::uint8_t data[8]{};
            static_cast<void>(node.send(j1939::Pgn{0xFF20}, data));
        });
        return;
    }
    case NetworkUse::LinMaster:
    {
        auto& master = ecu.lin_master(name, kBothWays);
        ecu.on_step([&master](SimTime) { master.set_frame_data(kFromMaster, std::array<std::uint8_t, 1>{0x5A}); });
        return;
    }
    case NetworkUse::LinSlave:
    {
        auto& slave = ecu.lin_slave(name);
        ecu.on_step([&slave](SimTime now) { slave.set_response(now, kFromSlave, std::array<std::uint8_t, 1>{0xA5}); });
        return;
    }
    case NetworkUse::Ethernet:
    {
        auto& channel = ecu.ethernet(name, {.mac = kSpeakerMac});
        ecu.on_step([&channel](SimTime) {
            std::array<std::uint8_t, eth::kHeaderBytes + 1> bytes{};
            std::copy(eth::kBroadcast.bytes.begin(), eth::kBroadcast.bytes.end(), bytes.begin());
            std::copy(kSpeakerMac.bytes.begin(), kSpeakerMac.bytes.end(), bytes.begin() + 6);
            bytes[12] = 0x88;
            bytes[13] = 0xB5;
            static_cast<void>(channel.send(*eth::Frame::from(bytes)));
        });
        return;
    }
    }
    throw unit::Failure("speak_on(): a NetworkUse outside the enumerators");
}

/// Puts `ecu` on the network `name` as what hears an ECU that speak_on(`use`) put there, and
/// counts in `heard` what it hears of it.
void hear_on(Ecu& ecu, NetworkUse use, const std::string& name, int& heard)
{
    switch (use)
    {
    case NetworkUse::Raw:
    case NetworkUse::J1939:
        ecu.can(name).on_frame([&heard](const can::TimedFrame&) { ++heard; });
        return;
    case NetworkUse::LinMaster:
        ecu.lin_slave(name, lin::LinSlaveOptions{{kFromMaster}, std::nullopt}).on_frame([&heard](const lin::LinReceivedFrame&) {
            ++heard;
        });
        return;
    case NetworkUse::LinSlave:
        // The slave only answers: a master hears it as the response to its header.
        ecu.lin_master(name, kBothWays).on_result([&heard](const lin::LinScheduleResult& r) {
            heard += r.id == kFromSlave && r.status == lin::LinResultStatus::Ok ? 1 : 0;
        });
        return;
    case NetworkUse::Ethernet:
        ecu.ethernet(name).on_frame([&heard](const eth::TimedFrame&) { ++heard; });
        return;
    }
    throw unit::Failure("hear_on(): a NetworkUse outside the enumerators");
}

struct Heard
{
    int in_simulation = 0; ///< by the ECU on the network "IN_SIMULATION"
    int in_code = 0;       ///< by the ECU on the network "IN_CODE"
};

/// Three ECUs on SIL Kit: one whose code speaks on "IN_CODE" with `use`, in a Simulation of its
/// own with `names` (as a process of its own would be), and one that hears on each of
/// "IN_SIMULATION" and "IN_CODE".
Heard heard_with(NetworkUse use, std::map<std::string, std::string> names)
{
    Heard heard;
    std::exception_ptr error;
    {
        Simulation sim(SilKitOptions{.other_participants = {"Speaker"}});
        hear_on(sim.add_ecu("OnSimulationName", 5ms), use, "IN_SIMULATION", heard.in_simulation);
        hear_on(sim.add_ecu("OnCodeName", 5ms), use, "IN_CODE", heard.in_code);
        Simulation other(SilKitOptions{.registry_uri = sim.registry_uri(), .controller = false, .network_names = std::move(names)});
        speak_on(other.add_ecu("Speaker", 5ms), use, "IN_CODE");
        {
            std::jthread speaker([&] {
                try
                {
                    other.run();
                }
                catch (...)
                {
                    error = std::current_exception();
                }
            });
            sim.run_for(300ms);
        }
    }
    if (error)
    {
        std::rethrow_exception(error);
    }
    return heard;
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

TEST_CASE(every_use_of_a_renamed_network_is_on_the_simulations_network)
{
    // Whatever makes the SIL Kit controller of a use must make it on the simulation's name: the
    // ECU on that network hears the speaker, and the one on the code's name hears nothing at all.
    // Without the other name it is the other way round (so the hearing itself works).
    for (const NetworkUse use : kNetworkUses)
    {
        const Heard renamed = heard_with(use, {{"IN_SIMULATION", "IN_CODE"}});
        if (renamed.in_simulation == 0 || renamed.in_code != 0)
        {
            throw unit::Failure(std::string(to_string(use)) + " with network_names {IN_SIMULATION: IN_CODE}: heard " +
                                std::to_string(renamed.in_simulation) + " on IN_SIMULATION (expected some) and " +
                                std::to_string(renamed.in_code) + " on IN_CODE (expected none)");
        }
        const Heard plain = heard_with(use, {});
        if (plain.in_simulation != 0 || plain.in_code == 0)
        {
            throw unit::Failure(std::string(to_string(use)) + " without network_names: heard " +
                                std::to_string(plain.in_simulation) + " on IN_SIMULATION (expected none) and " +
                                std::to_string(plain.in_code) + " on IN_CODE (expected some)");
        }
    }
}

TEST_CASE(networks_that_would_be_one_in_the_simulation_are_refused_in_any_order)
{
    // The code's "A" is "B" in the simulation, and the code has a "B" of its own: one bus for two
    // networks of the ECU, whatever their kinds and whichever is made first.
    for (const NetworkUse first : kNetworkUses)
    {
        for (const NetworkUse second : kNetworkUses)
        {
            for (const bool renamed_first : {true, false})
            {
                Simulation sim(SilKitOptions{.network_names = {{"B", "A"}}});
                Ecu& ecu = sim.add_ecu("E", 5ms);
                put_on(ecu, first, renamed_first ? "A" : "B");
                const std::string text = refusal([&] { put_on(ecu, second, renamed_first ? "B" : "A"); });
                if (text.find("network_names makes the network \"A\" of this ECU's code the network \"B\" of the "
                              "simulation, and the code has a network it names \"B\" itself, so two networks of the ECU "
                              "would be one bus.") == std::string::npos)
                {
                    throw unit::Failure(std::string(to_string(first)) + " then " + to_string(second) + ": " + text);
                }
            }
        }
    }
    // Two names that change places are two networks; a J1939 network and a raw channel of one
    // name of the code are one CAN network, as without other names.
    Simulation sim(SilKitOptions{.network_names = {{"B", "A"}, {"A", "B"}}});
    Ecu& ecu = sim.add_ecu("E", 5ms);
    ecu.j1939("A");
    static_cast<void>(ecu.can("A"));
    static_cast<void>(ecu.lin_slave("B"));
    CHECK_EQ(ecu.describe().networks.size(), std::size_t{3});
    CHECK(ecu.describe().networks[2].name == "B"); // the code's names
}

TEST_CASE(the_kind_of_a_network_across_ecus_is_checked_by_the_simulations_name)
{
    // Every ECU of the process gets the same names. B's code calls its LIN network "LIN_B", which
    // is "N" in the simulation; A's CAN network is named "N" by its code.
    Simulation sim(SilKitOptions{.network_names = {{"N", "LIN_B"}}});
    Ecu& a = sim.add_ecu("A", 5ms);
    Ecu& b = sim.add_ecu("B", 5ms);
    a.j1939("N"); // A has no "LIN_B": the entry is not for it, and nothing is refused
    check_text(refusal([&] { static_cast<void>(b.lin_slave("LIN_B")); }),
               "vehicle_sil::Simulation: lin_slave(\"LIN_B\") (the network \"N\" of the simulation) of the ECU \"B\": "
               "\"N\" is a CAN network of the ECU \"A\" (made by j1939() or can()); a LIN network needs a name of its own");
    // The code's name alone is no clash: A's "LIN_B" would be "N" too, but "X" is free.
    static_cast<void>(b.lin_slave("X"));
    // And the other way: a name of the code that another ECU uses for another kind is fine once
    // the simulation's names differ.
    Simulation other(SilKitOptions{.network_names = {{"BODY", "N"}}});
    Ecu& c = other.add_ecu("C", 5ms);
    c.j1939("N"); // "BODY" in the simulation
    Ecu& d = other.add_ecu("D", 5ms);
    check_text(refusal([&] { static_cast<void>(d.lin_slave("BODY")); }),
               "vehicle_sil::Simulation: lin_slave(\"BODY\") of the ECU \"D\": \"BODY\" is a CAN network of the ECU \"C\" "
               "(made by j1939() or can()); a LIN network needs a name of its own");
}
