// SPDX-License-Identifier: MIT
//
// Not built: read by the test launch.described_networks_check_rejects_a_direct_read. A rule that
// reads the networks as the executable names them, and the networks of the file, which it may.
bool fixture(const SimulationConfig& config, const ParticipantResult& result)
{
    for (const auto& network : result.ecu->description.networks)
    {
        if (network.name == config.networks.front().name)
        {
            return true;
        }
    }
    return config.networks.empty();
}
