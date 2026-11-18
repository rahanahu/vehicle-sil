// SPDX-License-Identifier: MIT
//
// How the Linux links decide Up or Down from an interface's flags and carrier. The kernel's side
// is held by the tests ethernet_bridge.netns.*.
#include "test_harness.hpp"

#include "linux/linux_link.hpp"

#include <net/if.h>

namespace {

using namespace vehicle_sil::eth_link;
using namespace vehicle_sil::eth_link::detail;

} // namespace

TEST_CASE(link_state_is_down_without_IFF_UP_whatever_the_carrier)
{
    CHECK(link_state(IFF_RUNNING, true) == LinkState::Down);
    CHECK(link_state(0, true) == LinkState::Down);
    CHECK(link_state(IFF_RUNNING, std::nullopt) == LinkState::Down);
}

TEST_CASE(link_state_follows_the_carrier_not_IFF_RUNNING_which_lags_it)
{
    CHECK(link_state(IFF_UP, true) == LinkState::Up);
    CHECK(link_state(IFF_UP | IFF_RUNNING, false) == LinkState::Down);
}

TEST_CASE(link_state_falls_back_to_IFF_RUNNING_when_the_driver_does_not_tell_the_carrier)
{
    CHECK(link_state(IFF_UP | IFF_RUNNING, std::nullopt) == LinkState::Up);
    CHECK(link_state(IFF_UP, std::nullopt) == LinkState::Down);
}
