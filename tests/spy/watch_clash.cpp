// SPDX-License-Identifier: MIT
//
// Does not build, on purpose (j1939_spy.watch_of_a_signal_named_count_does_not_build): a signal
// named "count" would give the watch two variables "<as>.count".
#include "hand_written_messages.hpp"

#include "vehicle_sil/spy/watch.hpp"

void watch_a_clash(vehicle_sil::spy::Spy& spy);
void watch_a_clash(vehicle_sil::spy::Spy& spy) { spy.watch<vehicle_sil::unit::spy::hand_written::Clash>("clash"); }
