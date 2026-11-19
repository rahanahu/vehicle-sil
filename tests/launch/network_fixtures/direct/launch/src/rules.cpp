// SPDX-License-Identifier: MIT
//
// Not built: read by the test launch.described_networks_check_rejects_a_direct_read. A rule that
// reads the networks as the executable names them (also through a pointer to the member), and the
// networks of the file, which it may.
bool fixture(const config::SimulationConfig& config, const config::ParticipantResult& result)
{
    if ((result.ecu->description.*(&EcuDescription::networks)).empty())
    {
        return false;
    }
    for (const auto& network : result.ecu->description.networks)
    {
        if (network.name == config.networks.front().name)
        {
            return true;
        }
    }
    return config.networks.empty();
}
