// SPDX-License-Identifier: MIT
//
// Not built: read by the test peer.controller_check_rejects_a_direct_name. A controller made
// outside the file that may make controllers, even with the simulation's name.
void fixture(SilKit::IParticipant& participant, const Names& names, const std::string& network)
{
    participant.CreateLinController(network, names.simulation_name(network));
}
