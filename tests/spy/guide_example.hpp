// SPDX-License-Identifier: MIT
//
// The example of docs/j1939-guide.md and docs/ja/j1939-guide.md ("Listen-only participants"),
// with cal_messages.yaml: the lines between "// guide: begin" and "// guide: end" are the guide's
// block (j1939_spy.guide_example_is_the_guides compares them), and test_spy_watch.cpp builds and
// runs them. Included by that one file only (it declares names at namespace scope).
#pragma once

#include "cal_messages.hpp"

#include "vehicle_sil/ecu/ecu.hpp"
#include "vehicle_sil/j1939/types.hpp"
#include "vehicle_sil/spy/spy.hpp"
#include "vehicle_sil/spy/watch.hpp"

#include <chrono>
#include <cstdint>

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

// guide: begin
namespace cal {
inline constexpr j1939::Pgn kPgn{0xEF00}; // Proprietary A: the group of Write and WriteAnswer (cal_messages.yaml)
}

class BlockWatch {                                   // a watcher: takes Scope& and its own settings; no base class, creates no Listener
public:
    BlockWatch(vehicle_sil::spy::Scope& s, j1939::Address from) {
        s.on_message(cal::kPgn, [this, from](const j1939::Message& m) {   // the Scope keeps the registration (no removal to write)
            if (m.source != from || m.time < latest_) { return; }        // the order of delivery can vary: compare times to find the newest
            latest_ = m.time; ++count_; bytes_ = static_cast<int>(m.data.size());
        });
        s.output("count", &count_); s.output("bytes", &bytes_, {.unit = "bytes"});   // becomes "block.count"
    }
private: vehicle_sil::SimTime latest_{}; std::int64_t count_ = 0; int bytes_ = 0;
};

class CalSpy {
public:
    explicit CalSpy(vehicle_sil::Ecu& ecu) : spy_(ecu, ecu.j1939("J1939_CAN")) {    // no node: sends nothing
        spy_.watch<cal::Write>("write", {.from = 0xF9, .to = 0x21});               // write.address, write.value, write.count ...
        spy_.watch<cal::WriteAnswer>("answer", {.from = 0x21, .stale_after = 500ms}); // answer.result ..., answer.stale
        spy_.add<BlockWatch>("block", j1939::Address{0x21});                        // block.count, block.bytes
    }                                                                               // the Spy writes spy.missed and its breakdown
private: vehicle_sil::spy::Spy spy_;
};
// guide: end
