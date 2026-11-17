// SPDX-License-Identifier: MIT
//
// The SIL Kit participant configuration that vehicle-sil makes or completes, and the registry's
// configuration (vehicle_sil::core).
#pragma once

#include "vehicle_sil/core/registry_uri.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil {

/// The SIL Kit participant configuration (JSON) for a participant of the registry at
/// `registry_uri` that is given none. It changes two of SIL Kit's defaults:
///
/// - A participant whose registry URI is a loopback address (is_loopback_registry_uri()) listens
///   for the other participants on loopback addresses only (Middleware.AcceptorUris: TCP on
///   127.0.0.1, or ::1 for an IPv6 ::1 registry URI, port 0; and, but on Windows, a local-domain
///   socket of a new name in the temporary directory), instead of on every address. Then no
///   other computer can connect to it, and Windows's firewall does not ask whether to let one.
///   Not when `listen_on_network` is true, or, when it is empty, when kListenOnNetworkVariable is
///   1: a registry that listens at 0.0.0.0 may be reached by other computers while this computer's
///   participants connect to it at 127.0.0.1. A `listen_on_network` that has a value decides alone
///   (the environment is not read).
/// - On Windows, the connections between the participants of one machine are TCP connections
///   (Middleware.EnableDomainSockets: false). With local-domain sockets there, closing the
///   socket to a process whose threads are suspended was seen not to return, and the closing
///   process stayed after it was told to end; with TCP connections that was not seen
///   (docs/dependencies.md says what was tried and what is a guess).
///
/// And for every registry: no dynamic simulation steps
/// (Experimental.TimeSynchronization.DynamicSimulationStep: false). Without the key, SIL Kit cuts
/// the steps of a participant down to the smallest step of the simulation as soon as another
/// participant asks for it, and the fixed step that ParticipantRuntime promises is gone.
///
/// Each call makes a new name for the local-domain socket. Throws std::invalid_argument for a value
/// of kListenOnNetworkVariable other than 1, 0 or empty, when it reads it.
[[nodiscard]] std::string default_participant_configuration(std::string_view registry_uri,
                                                            std::optional<bool> listen_on_network = std::nullopt);

/// default_participant_configuration() with more members of the top level: `members` is their
/// JSON without the braces (`"Logging":{...}`), or empty. For a program that makes a
/// configuration of its own, so that the defaults are made in one place. `members` must not
/// have "Middleware" or "Experimental".
[[nodiscard]] std::string default_participant_configuration_with(std::string_view registry_uri, std::string_view members,
                                                                 std::optional<bool> listen_on_network = std::nullopt);

/// The SIL Kit participant configuration (YAML or JSON text) that the participant
/// `participant_name` of the registry at `registry_uri` is made with, from the `configuration` it
/// is given (YAML or JSON text): default_participant_configuration() if that is "{}" or empty
/// (white space aside, or only comments); otherwise the given text with vehicle-sil's defaults
/// for what it leaves out.
///
/// - SIL Kit reads `configuration` first (its schema, its Includes; an included file is looked
///   for from the working directory and in Includes.SearchPathHints). What SIL Kit refuses is SIL
///   Kit's exception (SilKit::ConfigurationError), with SIL Kit's words; for a text that is not
///   YAML, " (at line <n>, column <n> of the configuration text)" is added to them. What the
///   configuration says is taken from that reading, so a member counts whichever file it is in.
/// - Middleware.AcceptorUris: the given ones; without them, as default_participant_configuration()
///   (loopback only for a loopback registry URI, unless `listen_on_network` or
///   kListenOnNetworkVariable says otherwise).
/// - Middleware.EnableDomainSockets on Windows: false, unless the configuration says false
///   itself. A `true` written there cannot be told from none (it is SIL Kit's default) and
///   becomes false as well.
/// - Experimental.TimeSynchronization.DynamicSimulationStep: false when it is left out.
/// - Everything else (Logging, HealthCheck, the other members of Middleware, ...) stays as it is
///   written: the members above are added to the given text, in its first file (a member with
///   nothing under it, as "Experimental:", is one that is left out). A configuration that leaves
///   none of them out is returned as it is.
///
/// std::invalid_argument, with what to change, for a configuration that would make the
/// participant another one than the program says: a ParticipantName other than
/// `participant_name`, a Middleware.RegistryUri other than `registry_uri` (SIL Kit takes the
/// configuration's value for both; the same value is accepted), and DynamicSimulationStep: true
/// (ParticipantRuntime runs on its own fixed step). An empty `registry_uri` means that it is not
/// known (a check before the run): the configuration's URI is not compared.
///
/// std::runtime_error if the text that SIL Kit accepted cannot be read again to add the members
/// (an error of vehicle-sil; the message says how to go on without it).
///
/// All of this needs the YAML library that SIL Kit builds (rapidyaml). In a build against a SIL
/// Kit that vehicle-sil did not build from source, a configuration that is not "{}" or empty is
/// returned as it is, unread: nothing is added, and none of the checks above is made (SIL Kit
/// reads it when the participant is made). It should then set Middleware.EnableDomainSockets to
/// false on Windows and DynamicSimulationStep to false (a ParticipantRuntime whose step SIL Kit
/// cut fails that step), it listens where it says, and it should not set ParticipantName or
/// Middleware.RegistryUri.
[[nodiscard]] std::string merged_participant_configuration(std::string_view configuration,
                                                           std::string_view participant_name,
                                                           std::string_view registry_uri,
                                                           std::optional<bool> listen_on_network = std::nullopt);

/// The controllers of `configuration` (a SIL Kit participant configuration, YAML or JSON text,
/// with what it includes) that are given a network: one text for each, e.g.
/// `CanControllers[0] (Name "CAN1"): Network "PowertrainCAN"`. SIL Kit puts such a controller on
/// that network whatever name the program gives when it makes the controller, so whoever renames
/// networks (PeerOptions::network_names) asks this first. Empty if there is none, for a text that
/// SIL Kit does not accept (whoever makes the participant says what is wrong with it), and in a
/// build without rapidyaml (see merged_participant_configuration(): the text is not read).
[[nodiscard]] std::vector<std::string> configured_controller_networks(std::string_view configuration);

/// Whether configured_controller_networks(configuration) can tell: false only in a build without
/// rapidyaml for a configuration that is not blank (its text is not read there, so a Network in
/// it would go unseen). Whoever renames networks refuses such a configuration.
[[nodiscard]] bool can_read_controller_networks(std::string_view configuration);

/// The text of the SIL Kit participant configuration file `path`, for
/// ParticipantRuntimeOptions::participant_configuration and the like. If the file has Includes,
/// its directory is added to Includes.SearchPathHints, so that the files it includes are found
/// beside it from any working directory (not in a build without rapidyaml, see
/// merged_participant_configuration(): they are then looked for from the working directory).
/// The text is not checked here (merged_participant_configuration() and ParticipantRuntime do):
/// a file that is not YAML, or whose Includes or SearchPathHints is not what SIL Kit wants, is
/// returned as it is, and SIL Kit says what is wrong when the text is used.
/// std::invalid_argument if there is no such file (with the directory a relative path was taken
/// from) or it cannot be read; nothing else is thrown for what the file says.
[[nodiscard]] std::string participant_configuration_from_file(const std::string& path);

/// The SIL Kit configuration (JSON) for a registry that vehicle-sil starts: without local-domain
/// sockets on every system (TCP only, so that StartListening() fails when its port is taken instead
/// of going on with a local-domain socket only), and without acceptors (a registry listens at the
/// URI it is started with, ISilKitRegistry::StartListening()).
[[nodiscard]] std::string_view default_registry_configuration() noexcept;

} // namespace vehicle_sil
