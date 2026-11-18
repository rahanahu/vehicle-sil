// SPDX-License-Identifier: MIT
#include "vehicle_sil/simulation/ecu_main.hpp"

#include "vehicle_sil/control/wire.hpp"
#include "vehicle_sil/control_silkit/agent_transport.hpp"
#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/ecu/j1939_node_argument.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include "ecu_main_command_line.hpp"
#include "ecu_main_describe_stdout.hpp"
#include "ecu_main_stop_on_signal.hpp"
#include "ecu_main_text.hpp"

#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {

namespace {

// The exit codes, as vehicle_sil_launch knows them (vehicle_sil/ecu/executable.hpp).
using ecu_executable::kExitFailed;
using ecu_executable::kExitInterrupted;
using ecu_executable::kExitOk;
using ecu_executable::kExitSetupFailed;
using ecu_executable::kExitUsage;

// The parts of ecu_main() in the other files of this directory.
using ecu_main_detail::asks_for_help;
using ecu_main_detail::CommandLine;
using ecu_main_detail::DescribeStdout;
using ecu_main_detail::hex_bytes;
using ecu_main_detail::in_quotes;
using ecu_main_detail::kDefaultStartTimeout;
using ecu_main_detail::one_line;
using ecu_main_detail::parse;
using ecu_main_detail::program_name;
using ecu_main_detail::StopOnSignal;
using ecu_main_detail::usage;
using ecu_main_detail::UsageError;
using ecu_main_detail::write_stdout;
#ifdef _WIN32
using ecu_main_detail::value_not_ascii;
#endif

/// --silkit-config: the configuration in `file` as JSON text for SilKitOptions, read and checked
/// by SIL Kit and by vehicle-sil for the participant `name` (`registry_uri` empty: --describe,
/// which knows none). With `renames` (--map-network changes a name), a configuration that puts a
/// controller on a network is refused: SIL Kit would use that network. A failure says the option
/// and the file.
std::string silkit_configuration(const std::string& file, const std::string& name, const std::string& registry_uri,
                                 bool renames = false)
{
    const std::string where = "--silkit-config " + in_quotes(file) + ": ";
    try
    {
        std::string text = participant_configuration_from_file(file);
        // Only the check: the participant makes its own (where it listens is decided there).
        static_cast<void>(merged_participant_configuration(text, name, registry_uri, false));
        if (renames)
        {
            if (!can_read_controller_networks(text))
            {
                throw std::invalid_argument(
                    "--map-network renames networks, and this build of vehicle-sil cannot read the configuration (it "
                    "was built against a SIL Kit that is not built from source): a controller that the configuration "
                    "puts on a Network would stay there unnoticed. Leave --silkit-config out, or leave --map-network "
                    "out and name the networks in the configuration");
            }
            if (const auto configured = configured_controller_networks(text); !configured.empty())
            {
                throw std::invalid_argument(
                    "the configuration puts a controller on a network itself (" + configured.front() +
                    "), and --map-network renames networks: SIL Kit would use the configuration's network, whatever "
                    "--map-network says. Remove Network from the controllers of the configuration, or leave "
                    "--map-network out and name the networks there");
            }
        }
        return text;
    }
    catch (const SilKit::ConfigurationError& e)
    {
        throw std::invalid_argument(where + "SIL Kit does not accept the configuration: " + e.what() +
                                    " (correct the file, or a file it includes; SIL Kit's documentation of the "
                                    "participant configuration lists the keys and their values)");
    }
    catch (const std::exception& e)
    {
        throw std::invalid_argument(where + e.what());
    }
}

/// `what` of a SimulationError of the ECU `name` without the "<name>: " it starts with.
std::string without_name(const std::string& what, const std::string& name)
{
    const std::string prefix = name + ": ";
    return what.compare(0, prefix.size(), prefix) == 0 ? what.substr(prefix.size()) : what;
}

/// Writes "error: <name>: <why>" as one line to stderr; returns `code`.
int fail(const std::string& name, int code, std::string_view why)
{
    std::cerr << "error: " << one_line(name) << ": " << one_line(why) << '\n' << std::flush;
    return code;
}

/// The addresses as the code gives them ("0x00, 0x21"; 254: "any (254)").
std::string coded_addresses_text(const std::vector<j1939::Address>& addresses)
{
    std::string text;
    for (const j1939::Address a : addresses)
    {
        text += (text.empty() ? "" : ", ") + j1939::coded_address_text(a);
    }
    return text;
}

/// Why no node took the --j1939-node `unused` (Peer::unused_j1939_nodes()), and what to do, for
/// the line of a failed set-up.
std::string unused_j1939_node_text(const UnusedJ1939Node& unused)
{
    const std::string argument =
        std::string(ecu_executable::kJ1939NodeArgument) + " " + ecu_executable::format_j1939_node_argument(unused.entry);
    const std::string network = "'" + unused.entry.network + "'";
    const auto& from = unused.entry.node.from;
    switch (unused.reason)
    {
    case UnusedJ1939Node::Reason::NoNetwork:
    {
        if (unused.networks.empty())
        {
            return argument + ": the ECU has no J1939 network: leave this --j1939-node out";
        }
        std::string names;
        const UnusedJ1939Node::Network* same = nullptr; // the first one that the simulation does not rename
        for (const auto& n : unused.networks)
        {
            names += (names.empty() ? "'" : ", '") + n.simulation + "'" +
                     (n.code == n.simulation ? "" : " (" + n.code + " in its code)");
            if (same == nullptr && n.code == n.simulation)
            {
                same = &n;
            }
        }
        std::string text = argument + ": the ECU has no J1939 network " + network + " (its J1939 networks: " + names +
                           "): write the simulation's name of one of them";
        if (same != nullptr)
        {
            text += "; if the simulation calls '" + same->code + "' " + network + ", give --map-network " +
                    unused.entry.network + "=" + same->code + " too";
        }
        return text;
    }
    case UnusedJ1939Node::Reason::NoNodes:
        return argument + ": the ECU has no node on " + network +
               " (nodes that it adds after it starts cannot be changed): leave this --j1939-node out";
    case UnusedJ1939Node::Reason::NoNodeAtFrom:
        return argument + ": the ECU has no node that its code puts at " +
               j1939::coded_address_text(from.value_or(j1939::kNullAddress)) + " on " + network + " (its nodes there: " +
               coded_addresses_text(unused.coded_addresses) +
               "; nodes that it adds after it starts cannot be changed): after from: goes the address in the ECU's code, "
               "not the new one; correct it, or leave this --j1939-node out";
    case UnusedJ1939Node::Reason::AddedAfterFirstStep:
        break;
    }
    // ecu_main() looks before any step, so only a node added in a step would get here.
    if (from)
    {
        return argument + ": the ECU's node at " + j1939::coded_address_text(*from) + " on " + network +
               " was added after the network's first step, so it keeps the values in its code: add the node before the "
               "ECU starts";
    }
    if (unused.coded_addresses.size() > 1)
    {
        return argument + ": the ECU's nodes on " + network + " (" + coded_addresses_text(unused.coded_addresses) +
               " in its code) were added after the network's first step, so they keep the values in their code: add "
               "the nodes before the ECU starts";
    }
    return argument + ": the ECU's node on " + network + " (" + coded_addresses_text(unused.coded_addresses) +
           " in its code) was added after the network's first step, so it keeps the values in its code: add the node "
           "before the ECU starts";
}

} // namespace

int ecu_main(int argc, const char* const* argv, const EcuMainOptions& options, const std::function<void(Ecu&)>& set_up)
{
    const std::string program = program_name(argc, argv, options.name);
    std::vector<std::string_view> args;
    for (int i = 1; i < argc && argv != nullptr; ++i)
    {
        args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    if (asks_for_help(args))
    {
        if (!write_stdout(usage(program, options)))
        {
            return fail(options.name, kExitSetupFailed, "could not write the usage to stdout (is it closed?)");
        }
        return kExitOk;
    }
    CommandLine c;
    try
    {
#ifdef _WIN32
        const std::string inCodePage = ": on Windows the arguments of an ECU executable arrive in the code page, which "
                                       "can turn such a name into another one without an error. ";
        if (const auto text = value_not_ascii(argc, argv, L"--map-network"))
        {
            throw UsageError("--map-network " + in_quotes(*text) + " has characters that are not ASCII" + inCodePage +
                             "Give the networks to rename ASCII names (in the simulation and in the ECU's code)");
        }
        if (const auto text = value_not_ascii(argc, argv, L"--j1939-node"))
        {
            throw UsageError("--j1939-node " + in_quotes(*text) + " has characters that are not ASCII" + inCodePage +
                             "Give the J1939 networks of the simulation ASCII names");
        }
#endif
        c = parse(args);
    }
    catch (const UsageError& e)
    {
        const int code = fail(options.name, kExitUsage, e.what());
        std::cerr << usage(program, options) << std::flush;
        return code;
    }
    const std::string name = c.name.value_or(options.name);

    std::optional<Simulation> sim;
    // --registry-uri, for an ECU with a control agent: its variables go to the simulation's hub
    // over SIL Kit. After `sim` (the ECU), so that it is destroyed before it; the run has stopped
    // the ECU by then, so its last word carries the writes that were cancelled.
    std::optional<control_silkit::AgentTransport> control;
    std::optional<StopOnSignal> stop_on_signal; // --registry-uri; destroyed before the ECU
    std::optional<DescribeStdout> describe_out; // --describe; destroyed before the ECU, never restored
    Ecu* ecu = nullptr;
    try
    {
        if (!detail::is_utf8(options.name))
        {
            throw std::invalid_argument("EcuMainOptions::name is not ASCII or UTF-8 text (the bytes " +
                                        hex_bytes(options.name) + "): give the ECU a UTF-8 name in main()");
        }
        if (c.registry_uri)
        {
            SilKitOptions silkit;
            silkit.registry_uri = *c.registry_uri;
            silkit.controller = false;
            silkit.start_timeout = c.start_timeout.value_or(kDefaultStartTimeout);
            bool renames = false;
            for (const auto& [simulation, executable] : c.network_map)
            {
                silkit.network_names.emplace(simulation, executable);
                renames = renames || simulation != executable;
            }
            if (!c.j1939_nodes.empty())
            {
                // For this ECU by its participant name (--name), the name add_ecu() gets.
                silkit.j1939_nodes[name] = c.j1939_nodes;
            }
            if (c.silkit_config)
            {
                silkit.participant_configuration = silkit_configuration(*c.silkit_config, name, *c.registry_uri, renames);
            }
            sim.emplace(silkit);
            stop_on_signal.emplace(name);
        }
        else
        {
            sim.emplace();
        }
        try
        {
            ecu = &sim->add_ecu(name, options.step);
        }
        catch (const std::logic_error&)
        {
            throw; // a bad name or step (std::invalid_argument is a logic_error)
        }
        catch (const NameInUseError& e)
        {
            if (!c.registry_uri)
            {
                throw;
            }
            // The registry was reached and refused the name: the message says so itself
            // (create_participant()).
            throw std::runtime_error("cannot join the simulation at " + in_quotes(*c.registry_uri) + ": " + e.what());
        }
        catch (const std::exception& e)
        {
            if (!c.registry_uri)
            {
                throw;
            }
            // SIL Kit: the participant connects to the registry here.
            throw std::runtime_error(
                "cannot join the simulation at " + in_quotes(*c.registry_uri) + ": " + e.what() +
                " (check that vehicle_sil_launch run (or the registry) is started first and waits for the "
                "participants; that the URI has the address of the computer it runs on (127.0.0.1 on this one) and "
                "the port of its registry_uri, e.g. silkit://192.0.2.10:8500; that a firewall there lets this "
                "computer in; and that no other participant there is named " + in_quotes(name) + ")");
        }
        if (stop_on_signal)
        {
            stop_on_signal->attach(sim->peer(name));
        }
        if (!set_up)
        {
            throw std::invalid_argument("no function to set the ECU up (ecu_main()'s set_up is empty)");
        }
        // --describe: from here on, stdout has only the JSON (DescribeStdout).
        if (c.describe)
        {
            describe_out.emplace();
            if (c.silkit_config)
            {
                // What --registry-uri would refuse is said now (vehicle_sil_launch check passes the
                // arguments of `command` to --describe). SIL Kit writes its warnings to stdout: after
                // describe_out, they go to stderr.
                static_cast<void>(silkit_configuration(*c.silkit_config, name, {}));
            }
        }
        set_up(*ecu);
        // Every ECU has a control agent, with the input ecu.connected (made now if the set-up
        // declared no variable).
        static_cast<void>(ecu->agent());
        // A --map-network for a network the ECU does not have is a mistake of this command line.
        // Checked here and not by the Peer: its options may serve several ECUs (Simulation).
        if (!c.network_map.empty())
        {
            const EcuDescription made = ecu->describe();
            for (const auto& [simulation, executable] : c.network_map)
            {
                bool found = false;
                std::string networks;
                for (const auto& network : made.networks)
                {
                    found = found || network.name == executable;
                    networks += (networks.empty() ? "'" : ", '") + network.name + "' (" + to_string(network.use) + ")";
                }
                if (!found)
                {
                    throw std::invalid_argument(
                        "--map-network " + simulation + "=" + executable + ": the ECU has no network '" + executable +
                        "' (its networks: " + (networks.empty() ? std::string("none") : networks) +
                        "): after the '=' goes the name that the ECU's code gives the network; correct it, or leave "
                        "this --map-network out");
                }
            }
        }
        // So is a --j1939-node that no node took (the first one). The nodes are all added now,
        // before any step: one that the ECU adds later keeps the values in its code.
        if (!c.j1939_nodes.empty()) // only with --registry-uri
        {
            if (const auto unused = sim->peer(name).unused_j1939_nodes(); !unused.empty())
            {
                throw std::invalid_argument(unused_j1939_node_text(unused.front()));
            }
        }
    }
    catch (const std::exception& e)
    {
        return fail(name, kExitSetupFailed, e.what());
    }
    catch (...)
    {
        return fail(name, kExitSetupFailed, "the set-up threw an exception that is not a std::exception");
    }
    if (c.registry_uri && !c.no_control)
    {
        // Every ECU has an agent (above); one that a transport of the application holds already
        // is left to that transport. Making the transport completes the agent's schema, as the
        // start would.
        try
        {
            Peer& peer = sim->peer(name);
            if (control::Agent* agent = peer.find_agent(); agent != nullptr && agent->transports() == 0)
            {
                control_silkit::AgentTransportOptions transport;
                transport.step = options.step;
                transport.last_word_wait = options.last_word_wait;
                control.emplace(*agent, peer.runtime(), transport);
            }
        }
        catch (const std::exception& e)
        {
            return fail(name, kExitSetupFailed, "could not start: " + std::string(e.what()));
        }
    }

    if (c.describe)
    {
        EcuDescription description = ecu->describe();
        // What --registry-uri does with the ECU's variables (if it has any): the same condition as
        // below. An agent that a transport of the application holds is left to that transport,
        // and is not announced.
        const bool own_transport = !description.variables.empty() && ecu->agent().transports() > 0;
        if (!c.no_control && !own_transport)
        {
            description.control = ControlOffer{"silkit", static_cast<int>(control::wire::kVersion),
                                               static_cast<int>(control::wire::kRevision)};
        }
        // What this executable takes from whoever starts it (the same for every way to call it).
        description.features = {std::string(ecu_executable::kFeatureMapNetwork),
                                 std::string(ecu_executable::kFeatureJ1939Node)};
        if (!describe_out->write(to_json(description)))
        {
            return fail(name, kExitSetupFailed, "could not write the description to stdout (is it closed?)");
        }
        return kExitOk;
    }
    try
    {
        if (c.local)
        {
            sim->run_for(*c.run_for);
        }
        else
        {
            struct Returned
            {
                StopOnSignal* stop;
                ~Returned()
                {
                    if (stop != nullptr)
                    {
                        stop->run_returned();
                    }
                }
            } const returned{stop_on_signal ? &*stop_on_signal : nullptr};
            sim->run();
        }
    }
    catch (const SimulationError& e)
    {
        if (stop_on_signal && !stop_on_signal->stopped_by().empty())
        {
            return fail(name, kExitInterrupted,
                        "interrupted (" + stop_on_signal->stopped_by() + "): it stopped the simulation and left it (" +
                            without_name(e.what(), name) + ")");
        }
        return fail(name, kExitFailed, without_name(e.what(), name));
    }
    catch (const StartTimeoutError& e)
    {
        return fail(name, kExitSetupFailed,
                    "the simulation did not start: " + std::string(e.what()) +
                        " (is a controller such as vehicle_sil_sim_controller running, with " + in_quotes(name) +
                        " among its required participants? To wait longer: --start-timeout-ms)");
    }
    catch (const std::exception& e)
    {
        if (c.local)
        {
            return fail(name, kExitFailed, e.what());
        }
        return fail(name, kExitSetupFailed, "could not start: " + std::string(e.what()));
    }
    catch (...)
    {
        return fail(name, kExitFailed, "the run threw an exception that is not a std::exception");
    }
    if (stop_on_signal && !stop_on_signal->stopped_by().empty())
    {
        return fail(name, kExitInterrupted,
                    "interrupted (" + stop_on_signal->stopped_by() + "): it stopped the simulation and left it");
    }
    return kExitOk;
}

} // namespace vehicle_sil
