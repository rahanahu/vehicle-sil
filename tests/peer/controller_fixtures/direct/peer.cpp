// SPDX-License-Identifier: MIT
//
// Not built: read by the test peer.controller_check_rejects_a_direct_name. A controller made on
// the name that the code gives, in the file that may make controllers.
void fixture(SilKit::IParticipant& participant, const std::string& network)
{
    participant.CreateCanController(network, network);
}
