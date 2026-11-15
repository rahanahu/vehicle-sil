// SPDX-License-Identifier: MIT
//
// The bytes of version 2, revision 0, of the messages between a control agent and a hub
// (vehicle_sil::control::wire), written out by hand, and the structures they carry. ECUs built
// with this revision send exactly these bytes. A later revision adds fields at the end of an
// element or of a message and counts the revision up; it must still read these bytes, and write
// them to a peer of revision 0 but for the revision byte and what it added. Do not regenerate
// them from the encoder; a change of what is here is a change of the format. When a revision adds
// a field, pin its bytes beside these (a new array), and leave these as they are.
//
// Every number is little endian. A text is its length (u32) and its bytes. Every message begins
// with a header of 8 bytes: 'V' 'S' 'C' 'W', the version (u16), the kind (u8) and the revision
// (u8). A variable, a write, a result and an event begin with their size (u32: the bytes that
// follow). Beside that, version 2 differs from version 1 (wire_v1_golden.hpp, the same messages)
// in two numbers: how many writes of a command took effect (in a result) and which write a
// refusal names (in a submitted message), each 0xFFFFFFFF for "not said".
#pragma once

#include "vehicle_sil/control/wire.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace vehicle_sil::control::wire_v2_golden {

/// The session of every message below (8 different bytes, so that their order shows).
inline constexpr std::uint64_t kSession = 0x0123456789ABCDEFULL;

inline SimTime at_ns(std::int64_t ns)
{
    return SimTime{SimDuration{ns}};
}

// ---------------------------------------------------------------------------------------------
// Hello (kind 1): a participant with one variable of each type; the first has a minimum, a
// maximum and a step, the third only a maximum, the enum has items (one of them negative).

inline wire::Hello hello()
{
    wire::Hello h;
    h.participant = "Engine";
    h.session = kSession;
    h.virtual_time = true;
    h.step = SimDuration{1'000'000};           // 1 ms
    h.sample_period = SimDuration{10'000'000}; // 10 ms
    h.max_pending = 256;
    VariableInfo coolant;
    coolant.name = "coolant";
    coolant.type = ValueType::Real;
    coolant.causality = Causality::Output;
    coolant.unit = "degC";
    coolant.min = -40.0;
    coolant.max = 100.0;
    coolant.step = 0.5;
    coolant.description = "water";
    VariableInfo gear;
    gear.name = "gear";
    gear.type = ValueType::Enum;
    gear.causality = Causality::Input;
    gear.items = {{"P", 0}, {"R", -1}, {"D", 1}};
    VariableInfo gain;
    gain.name = "gain";
    gain.type = ValueType::Integer;
    gain.causality = Causality::Parameter;
    gain.max = 2.0;
    VariableInfo on;
    on.name = "on";
    on.type = ValueType::Bool;
    on.causality = Causality::Input;
    h.schema = {coolant, gear, gain, on};
    return h;
}

inline constexpr std::uint8_t kHello[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x01,                                           // kind: hello
    0x00,                                           // revision 0
    0x06, 0x00, 0x00, 0x00,                         // participant: 6 bytes
    'E', 'n', 'g', 'i', 'n', 'e',                   //   "Engine"
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x01,                                           // virtual time: yes
    0x40, 0x42, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, // step: 1'000'000 ns
    0x80, 0x96, 0x98, 0x00, 0x00, 0x00, 0x00, 0x00, // sample period: 10'000'000 ns
    0x00, 0x01, 0x00, 0x00,                         // max pending: 256
    0x04, 0x00, 0x00, 0x00,                         // variables: 4
    // variable 1
    0x3B, 0x00, 0x00, 0x00,                         // size: 59 bytes
    0x07, 0x00, 0x00, 0x00,                         // name: 7 bytes
    'c', 'o', 'o', 'l', 'a', 'n', 't',              //   "coolant"
    0x02,                                           // type: real
    0x01,                                           // causality: output
    0x04, 0x00, 0x00, 0x00,                         // unit: 4 bytes
    'd', 'e', 'g', 'C',                             //   "degC"
    0x07,                                           // has: 1 min | 2 max | 4 step
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x44, 0xC0, // min: -40.0 (f64)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x59, 0x40, // max: 100.0 (f64)
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0x3F, // step: 0.5 (f64)
    0x05, 0x00, 0x00, 0x00,                         // description: 5 bytes
    'w', 'a', 't', 'e', 'r',                        //   "water"
    0x00, 0x00, 0x00, 0x00,                         // enum items: 0
    // variable 2
    0x3E, 0x00, 0x00, 0x00,                         // size: 62 bytes
    0x04, 0x00, 0x00, 0x00,                         // name: 4 bytes
    'g', 'e', 'a', 'r',                             //   "gear"
    0x03,                                           // type: enum
    0x00,                                           // causality: input
    0x00, 0x00, 0x00, 0x00,                         // unit: 0 bytes
    0x00,                                           // has: nothing
    0x00, 0x00, 0x00, 0x00,                         // description: 0 bytes
    0x03, 0x00, 0x00, 0x00,                         // enum items: 3
    0x01, 0x00, 0x00, 0x00, 'P',                    //   name "P"
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   value 0 (i64)
    0x01, 0x00, 0x00, 0x00, 'R',                    //   name "R"
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, //   value -1 (i64)
    0x01, 0x00, 0x00, 0x00, 'D',                    //   name "D"
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   value 1 (i64)
    // variable 3
    0x1F, 0x00, 0x00, 0x00,                         // size: 31 bytes
    0x04, 0x00, 0x00, 0x00,                         // name: 4 bytes
    'g', 'a', 'i', 'n',                             //   "gain"
    0x01,                                           // type: integer
    0x02,                                           // causality: parameter
    0x00, 0x00, 0x00, 0x00,                         // unit: 0 bytes
    0x02,                                           // has: 2 max
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, // max: 2.0 (f64)
    0x00, 0x00, 0x00, 0x00,                         // description: 0 bytes
    0x00, 0x00, 0x00, 0x00,                         // enum items: 0
    // variable 4
    0x15, 0x00, 0x00, 0x00,                         // size: 21 bytes
    0x02, 0x00, 0x00, 0x00,                         // name: 2 bytes
    'o', 'n',                                       //   "on"
    0x00,                                           // type: bool
    0x00,                                           // causality: input
    0x00, 0x00, 0x00, 0x00,                         // unit: 0 bytes
    0x00,                                           // has: nothing
    0x00, 0x00, 0x00, 0x00,                         // description: 0 bytes
    0x00, 0x00, 0x00, 0x00,                         // enum items: 0
};

// ---------------------------------------------------------------------------------------------
// Command (kind 2), with a time: one write of each value type.

inline wire::CommandMessage command_at()
{
    wire::CommandMessage m;
    m.session = kSession;
    m.command.seq = 5;
    m.command.at = at_ns(1'500'000); // 1.5 ms
    m.command.late = LatePolicy::ApplyNext;
    m.command.writes = {{"on", Value{true}}, {"gear", Value{std::int64_t{-1}}}, {"coolant", Value{1.5}}};
    return m;
}

inline constexpr std::uint8_t kCommandAt[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x02,                                           // kind: command
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 5
    0x01,                                           // has a time: yes
    0x60, 0xE3, 0x16, 0x00, 0x00, 0x00, 0x00, 0x00, // at: 1'500'000 ns
    0x01,                                           // late: apply in the next step
    0x03, 0x00, 0x00, 0x00,                         // writes: 3
    0x08, 0x00, 0x00, 0x00,                         // size: 8 bytes (write 1)
    0x02, 0x00, 0x00, 0x00,                         // name: 2 bytes
    'o', 'n',                                       //   "on"
    0x00,                                           // value type: bool
    0x01,                                           //   true
    0x11, 0x00, 0x00, 0x00,                         // size: 17 bytes (write 2)
    0x04, 0x00, 0x00, 0x00,                         // name: 4 bytes
    'g', 'e', 'a', 'r',                             //   "gear"
    0x01,                                           // value type: integer
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, //   -1 (i64)
    0x14, 0x00, 0x00, 0x00,                         // size: 20 bytes (write 3)
    0x07, 0x00, 0x00, 0x00,                         // name: 7 bytes
    'c', 'o', 'o', 'l', 'a', 'n', 't',              //   "coolant"
    0x02,                                           // value type: real
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F, //   1.5 (f64)
};

// Command (kind 2), without a time (the next step): the time field is there and 0.

inline wire::CommandMessage command_next_step()
{
    wire::CommandMessage m;
    m.session = kSession;
    m.command.seq = 6;
    m.command.late = LatePolicy::Reject;
    m.command.writes = {{"gain", Value{std::int64_t{2}}}};
    return m;
}

inline constexpr std::uint8_t kCommandNextStep[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x02,                                           // kind: command
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 6
    0x00,                                           // has a time: no
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // at: 0
    0x00,                                           // late: reject
    0x01, 0x00, 0x00, 0x00,                         // writes: 1
    0x11, 0x00, 0x00, 0x00,                         // size: 17 bytes (write 1)
    0x04, 0x00, 0x00, 0x00,                         // name: 4 bytes
    'g', 'a', 'i', 'n',                             //   "gain"
    0x01,                                           // value type: integer
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   2 (i64)
};

// ---------------------------------------------------------------------------------------------
// Submitted (kind 3): what Agent::submit() returned for a command, with the write it refused
// (the first one, 0), and without one.

inline wire::SubmittedMessage submitted()
{
    return wire::SubmittedMessage{kSession, 6, SubmitResult{SubmitStatus::Invalid, "out of range", 0}};
}

inline wire::SubmittedMessage submitted_without_a_write()
{
    return wire::SubmittedMessage{kSession, 6, SubmitResult{SubmitStatus::Invalid, "out of range", std::nullopt}};
}

inline constexpr std::uint8_t kSubmitted[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x03,                                           // kind: submitted
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 6
    0x02,                                           // status: invalid
    0x0C, 0x00, 0x00, 0x00,                         // reason: 12 bytes
    'o', 'u', 't', ' ', 'o', 'f', ' ',              //   "out of range"
    'r', 'a', 'n', 'g', 'e',                        //   (continued)
    0x00, 0x00, 0x00, 0x00,                         // failed write: 0
};

inline constexpr std::uint8_t kSubmittedWithoutAWrite[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x03,                                           // kind: submitted
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 6
    0x02,                                           // status: invalid
    0x0C, 0x00, 0x00, 0x00,                         // reason: 12 bytes
    'o', 'u', 't', ' ', 'o', 'f', ' ',              //   "out of range"
    'r', 'a', 'n', 'g', 'e',                        //   (continued)
    0xFF, 0xFF, 0xFF, 0xFF,                         // failed write: not said
};

// ---------------------------------------------------------------------------------------------
// Step (kind 4): results, an event and a sample of every value type, in one message.

inline constexpr std::size_t kStepMaxBytes = wire::kMaxMessageBytes;

inline std::vector<CommandResult> step_results()
{
    return {
        {5, CommandStatus::Applied, at_ns(2'000'000), true, {}, 3},
        {6, CommandStatus::Rejected, std::nullopt, false, "read only", 1},
    };
}

inline std::vector<Event> step_events()
{
    return {{at_ns(2'000'000), EventLevel::Warning, "hot"}};
}

inline Snapshot step_sample()
{
    return {at_ns(2'000'000), 3, {Value{true}, Value{std::int64_t{1}}, Value{1.5}}};
}

inline constexpr SimTime step_time()
{
    return SimTime{SimDuration{2'000'000}};
}

inline constexpr std::uint8_t kStep[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x04,                                           // kind: step
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // time: 2'000'000 ns
    0x02,                                           // flags: 2 values (not 1 final)
    0x02, 0x00,                                     // results: 2 (u16)
    0x01, 0x00,                                     // events: 1 (u16)
    // result 1
    0x1A, 0x00, 0x00, 0x00,                         // size: 26 bytes
    0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 5
    0x00,                                           // status: applied
    0x03,                                           // flags: 1 late | 2 applied at
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // applied at: 2'000'000 ns
    0x03, 0x00, 0x00, 0x00,                         // applied writes: 3
    0x00, 0x00, 0x00, 0x00,                         // reason: 0 bytes
    // result 2
    0x1B, 0x00, 0x00, 0x00,                         // size: 27 bytes
    0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 6
    0x02,                                           // status: rejected
    0x00,                                           // flags: none
    0x01, 0x00, 0x00, 0x00,                         // applied writes: 1
    0x09, 0x00, 0x00, 0x00,                         // reason: 9 bytes
    'r', 'e', 'a', 'd', ' ', 'o', 'n', 'l', 'y',    //   "read only"
    // event 1
    0x10, 0x00, 0x00, 0x00,                         // size: 16 bytes
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // time: 2'000'000 ns
    0x01,                                           // level: warning
    0x03, 0x00, 0x00, 0x00,                         // text: 3 bytes
    'h', 'o', 't',                                  //   "hot"
    // values
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // sample time: 2'000'000 ns
    0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sample sequence: 3
    0x03, 0x00, 0x00, 0x00,                         // total: 3
    0x00, 0x00, 0x00, 0x00,                         // first: 0
    0x03, 0x00, 0x00, 0x00,                         // count: 3
    0x00, 0x01,                                     // bool: true
    0x01,                                           // integer:
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   1 (i64)
    0x02,                                           // real:
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0x3F, //   1.5 (f64)
};

// ---------------------------------------------------------------------------------------------
// Step (kind 4) split at kMinMessageBytes: a sample of kSplitValues integers (0, 1, 2, ...).
// The first message holds kSplitFirstCount of them (2046 bytes: kSplitFirstHead, then the values
// as 0x01 and an i64 each); the second holds the rest. Values are no elements: but for the
// version, these are the bytes of version 1.

inline constexpr std::size_t kSplitMaxBytes = wire::kMinMessageBytes;
inline constexpr std::uint32_t kSplitValues = 223;
inline constexpr std::uint32_t kSplitFirstCount = 221;

inline Snapshot split_sample()
{
    Snapshot s{at_ns(2'000'000), 4, {}};
    for (std::uint32_t i = 0; i < kSplitValues; ++i)
    {
        s.values.emplace_back(std::int64_t{i});
    }
    return s;
}

inline constexpr std::uint8_t kSplitFirstHead[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x04,                                           // kind: step
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // time: 2'000'000 ns
    0x02,                                           // flags: 2 values
    0x00, 0x00,                                     // results: 0
    0x00, 0x00,                                     // events: 0
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // sample time: 2'000'000 ns
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sample sequence: 4
    0xDF, 0x00, 0x00, 0x00,                         // total: 223
    0x00, 0x00, 0x00, 0x00,                         // first: 0
    0xDD, 0x00, 0x00, 0x00,                         // count: 221
};

inline constexpr std::uint8_t kSplitSecond[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x04,                                           // kind: step
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // time: 2'000'000 ns
    0x02,                                           // flags: 2 values
    0x00, 0x00,                                     // results: 0
    0x00, 0x00,                                     // events: 0
    0x80, 0x84, 0x1E, 0x00, 0x00, 0x00, 0x00, 0x00, // sample time: 2'000'000 ns
    0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // sample sequence: 4
    0xDF, 0x00, 0x00, 0x00,                         // total: 223
    0xDD, 0x00, 0x00, 0x00,                         // first: 221
    0x02, 0x00, 0x00, 0x00,                         // count: 2
    0x01,                                           // integer:
    0xDD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   221 (i64)
    0x01,                                           // integer:
    0xDE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, //   222 (i64)
};

// ---------------------------------------------------------------------------------------------
// Step (kind 4), the agent's last word: a cancelled command and nothing else, with the final
// flag. A result that does not say how many writes took effect.

inline std::vector<CommandResult> final_results()
{
    return {{7, CommandStatus::Cancelled, std::nullopt, false, {}}};
}

inline constexpr SimTime final_time()
{
    return SimTime{SimDuration{3'000'000}};
}

inline constexpr std::uint8_t kFinalStep[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x04,                                           // kind: step
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
    0xC0, 0xC6, 0x2D, 0x00, 0x00, 0x00, 0x00, 0x00, // time: 3'000'000 ns
    0x01,                                           // flags: 1 final
    0x01, 0x00,                                     // results: 1
    0x00, 0x00,                                     // events: 0
    0x12, 0x00, 0x00, 0x00,                         // size: 18 bytes
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, // seq: 7
    0x03,                                           // status: cancelled
    0x00,                                           // flags: none
    0xFF, 0xFF, 0xFF, 0xFF,                         // applied writes: not said
    0x00, 0x00, 0x00, 0x00,                         // reason: 0 bytes
};

// ---------------------------------------------------------------------------------------------
// Probe (kind 5), ready (kind 6) and last word received (kind 7): the session and nothing else.

inline constexpr std::uint8_t kProbe[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x05,                                           // kind: probe
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
};

inline constexpr std::uint8_t kReady[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x06,                                           // kind: ready
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
};

inline constexpr std::uint8_t kLastWordReceived[] = {
    'V', 'S', 'C', 'W',                             // magic
    0x02, 0x00,                                     // version 2
    0x07,                                           // kind: last word received
    0x00,                                           // revision 0
    0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, // session
};

} // namespace vehicle_sil::control::wire_v2_golden
