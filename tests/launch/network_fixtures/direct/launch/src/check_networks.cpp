// SPDX-License-Identifier: MIT
//
// Not built: read by the test launch.described_networks_check_rejects_a_direct_read. The file
// that may read the networks of a description.
std::size_t fixture(const ReportedEcu& ecu) { return ecu.description.networks.size(); }
