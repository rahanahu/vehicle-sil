// SPDX-License-Identifier: MIT
//
// Where the bridge executable gets its external link from. The executable links
// os_link_factory.cpp (the links of this operating system); the process tests link a factory of
// fake links instead (tests/ethernet_bridge/fake_link_factory.cpp).
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_options.hpp"

#include <memory>
#include <string>

namespace vehicle_sil::ethernet_bridge {

/// The link `options` ask for (--backend, --interface, --promiscuous or --no-promiscuous, --tap-create), not
/// started: nothing is opened yet. Throws BackendUnavailableError (bridge_app.hpp) that says what
/// is wrong and how to fix it if the backend cannot be used here.
[[nodiscard]] std::unique_ptr<eth_link::ExternalLink> make_external_link(const BridgeOptions& options);

/// What --list-interfaces prints for options.backend: the text, or with --json (options.json)
/// the JSON of interface_list.hpp. Throws as make_external_link() does.
[[nodiscard]] std::string interface_list(const BridgeOptions& options);

} // namespace vehicle_sil::ethernet_bridge
