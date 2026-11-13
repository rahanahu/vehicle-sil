// SPDX-License-Identifier: MIT
// Fixture of core.silkit_configuration_check_rejects_a_direct_text:
// VehicleSilCheckSilKitConfiguration.cmake must reject it. Never compiled.
auto fixture()
{
    return SilKit::Config::ParticipantConfigurationFromString("{}");
}
