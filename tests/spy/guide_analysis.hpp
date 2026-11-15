// SPDX-License-Identifier: MIT
//
// The analysis example of docs/j1939-guide.md and docs/ja/j1939-guide.md ("Listen-only
// participants"), after the guide's first example (guide_example.hpp, whose declarations it uses):
// the lines between "// guide: begin" and "// guide: end" are the guide's block
// (j1939_spy.guide_example_is_the_guides compares them), and test_spy_analysis.cpp builds and runs
// them.
#pragma once

#include "guide_example.hpp"

#include <cstdint>
#include <optional>

// guide: begin
class WriteCheck {                  // judges each Write of the tool 0xF9 to the ECU 0x21 by the ECU's WriteAnswer
public:
    WriteCheck(vehicle_sil::spy::Scope& s, vehicle_sil::SimDuration limit) : limit_(limit) {
        s.on_message(cal::kPgn, [this](const j1939::Message& m) {          // in the order heard; also long messages
            if (const auto w = cal::Write::decode(m.data); w && m.source == 0xF9 && m.destination == 0x21) {
                if (asked_) { ++no_answer_; }                               // a new Write before the answer
                asked_ = w; asked_at_ = m.time;
            } else if (const auto a = cal::WriteAnswer::decode(m.data); a && asked_ && m.source == 0x21 && m.destination == 0xF9) {
                ++answered_; answer_ms_ = (m.time - asked_at_) / 1ms;
                if (a->address.raw() != asked_->address.raw() || a->value.raw() != asked_->value.raw()) { ++mismatches_; }
                asked_.reset();
            }
        });
        s.on_step([this](vehicle_sil::SimTime now) {                        // every step, after its messages
            if (asked_ && now - asked_at_ > limit_) { ++no_answer_; asked_.reset(); }
        });
        s.output("answered", &answered_); s.output("no_answer", &no_answer_); s.output("mismatches", &mismatches_);
        s.output("answer_ms", &answer_ms_, {.unit = "ms", .description = "Time to the latest answer (-1: none yet)"});
    }
private:
    vehicle_sil::SimDuration limit_;
    std::optional<cal::Write> asked_{};             // the Write waiting for its answer
    vehicle_sil::SimTime asked_at_{};
    std::int64_t answered_ = 0, no_answer_ = 0, mismatches_ = 0, answer_ms_ = -1;
};
// guide: end
