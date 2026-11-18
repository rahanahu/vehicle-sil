// SPDX-License-Identifier: MIT
//
// Version 2 of the messages between a control agent and a hub (control/wire.hpp): that another
// version is not read, what a reader skips from a newer revision and refuses from its own, and
// that the sizes the encoder counts with are the sizes it writes.
//
// The messages "of a newer revision" are written by hand here (Raw), not by the library's
// encoders: no encoder of this build writes them.
#include "test_harness.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
namespace wire = vehicle_sil::control::wire;

constexpr std::size_t kRevisionAt = 7;
/// "No number" in an optional u32.
constexpr std::uint32_t kNone = 0xFFFF'FFFF;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

std::optional<std::uint32_t> number(std::uint32_t n)
{
    return n;
}

/// A message written field by field.
struct Raw
{
    wire::Bytes bytes;

    Raw(wire::Kind kind, std::uint8_t revision)
        : bytes{'V', 'S', 'C', 'W', static_cast<std::uint8_t>(wire::kVersion), static_cast<std::uint8_t>(wire::kVersion >> 8),
                static_cast<std::uint8_t>(kind), revision}
    {
    }
    Raw& u8(std::uint8_t v)
    {
        bytes.push_back(v);
        return *this;
    }
    Raw& u16(std::uint16_t v)
    {
        return u8(static_cast<std::uint8_t>(v)).u8(static_cast<std::uint8_t>(v >> 8));
    }
    Raw& u32(std::uint32_t v)
    {
        return u16(static_cast<std::uint16_t>(v)).u16(static_cast<std::uint16_t>(v >> 16));
    }
    Raw& u64(std::uint64_t v)
    {
        return u32(static_cast<std::uint32_t>(v)).u32(static_cast<std::uint32_t>(v >> 32));
    }
    Raw& f64(double v)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        return u64(bits);
    }
    Raw& text(std::string_view v)
    {
        u32(static_cast<std::uint32_t>(v.size()));
        bytes.insert(bytes.end(), v.begin(), v.end());
        return *this;
    }
    /// An element: its size, then what `content` writes.
    template <typename Content>
    Raw& element(Content content)
    {
        const std::size_t size_at = bytes.size();
        u32(0);
        content(*this);
        const auto size = static_cast<std::uint32_t>(bytes.size() - size_at - 4);
        for (std::size_t i = 0; i < 4; ++i)
        {
            bytes[size_at + i] = static_cast<std::uint8_t>(size >> (8 * i));
        }
        return *this;
    }
};

/// What a later revision might add, in each place it may add something.
struct Added
{
    bool to_elements = false; ///< bytes at the end of every element
    bool to_message = false;  ///< bytes at the end of the message
    bool flags = false;       ///< a flag bit this revision does not know, wherever there are flags
};

void more(Raw& raw, bool add)
{
    if (add)
    {
        raw.u8(0xAA).u32(0xDEADBEEF);
    }
}

constexpr std::uint8_t kUnknownFlag = 0x80;

wire::Hello small_hello()
{
    wire::Hello hello;
    hello.participant = "Engine";
    hello.session = 0x1122334455667788ULL;
    hello.virtual_time = true;
    hello.step = 1ms;
    hello.sample_period = 10ms;
    hello.max_pending = 8;
    VariableInfo speed;
    speed.name = "speed";
    speed.type = ValueType::Real;
    speed.causality = Causality::Output;
    speed.unit = "rpm";
    speed.min = 0.0;
    speed.max = 8000.0;
    speed.description = "the speed";
    VariableInfo gear;
    gear.name = "gear";
    gear.type = ValueType::Enum;
    gear.causality = Causality::Input;
    gear.items = {{"P", 0}, {"D", 3}};
    hello.schema = {speed, gear};
    return hello;
}

/// small_hello() in version 2 (from revision 1 on, each variable ends with its availability: 0,
/// it does not report).
Raw raw_hello(std::uint8_t revision, Added added)
{
    Raw raw(wire::Kind::Hello, revision);
    raw.text("Engine").u64(0x1122334455667788ULL).u8(1).u64(1'000'000).u64(10'000'000).u32(8).u32(2);
    const auto availability = [&](Raw& r) {
        if (revision >= 1)
        {
            r.u8(0);
        }
    };
    raw.element([&](Raw& r) {
        r.text("speed").u8(2).u8(1).text("rpm").u8(static_cast<std::uint8_t>(1 | 2 | (added.flags ? kUnknownFlag : 0)));
        r.f64(0.0).f64(8000.0).text("the speed").u32(0);
        availability(r);
        more(r, added.to_elements);
    });
    raw.element([&](Raw& r) {
        r.text("gear").u8(3).u8(0).text("").u8(0).text("").u32(2).text("P").u64(0).text("D").u64(3);
        availability(r);
        more(r, added.to_elements);
    });
    more(raw, added.to_message);
    return raw;
}

wire::CommandMessage small_command()
{
    wire::CommandMessage m;
    m.session = 77;
    m.command.seq = 5;
    m.command.at = at(2ms);
    m.command.late = LatePolicy::ApplyNext;
    m.command.writes = {{"on", Value{true}}, {"gear", Value{std::int64_t{-3}}}, {"speed", Value{1.5}}};
    return m;
}

/// small_command() in version 2.
Raw raw_command(std::uint8_t revision, Added added)
{
    Raw raw(wire::Kind::Command, revision);
    raw.u64(77).u64(5).u8(1).u64(2'000'000).u8(1).u32(3);
    raw.element([&](Raw& r) {
        r.text("on").u8(0).u8(1);
        more(r, added.to_elements);
    });
    raw.element([&](Raw& r) {
        r.text("gear").u8(1).u64(static_cast<std::uint64_t>(std::int64_t{-3}));
        more(r, added.to_elements);
    });
    raw.element([&](Raw& r) {
        r.text("speed").u8(2).f64(1.5);
        more(r, added.to_elements);
    });
    more(raw, added.to_message);
    return raw;
}

/// A submitted message in version 2: session 9, seq 42, Invalid, "gear: no", about write 1.
Raw raw_submitted(std::uint8_t revision, Added added)
{
    Raw raw(wire::Kind::Submitted, revision);
    raw.u64(9).u64(42).u8(2).text("gear: no").u32(1);
    more(raw, added.to_message);
    return raw;
}

/// A step message in version 2: session 11, time 3 ms, two results, one event, and with
/// `values` a sample of two values.
Raw raw_step(std::uint8_t revision, Added added, bool values)
{
    Raw raw(wire::Kind::Step, revision);
    raw.u64(11).u64(3'000'000).u8(static_cast<std::uint8_t>((values ? 2 : 0) | (added.flags ? kUnknownFlag : 0))).u16(2).u16(1);
    raw.element([&](Raw& r) {
        // Applied late at 3 ms, both writes.
        r.u64(1).u8(0).u8(static_cast<std::uint8_t>(1 | 2 | (added.flags ? kUnknownFlag : 0))).u64(3'000'000).u32(2).text("");
        more(r, added.to_elements);
    });
    raw.element([&](Raw& r) {
        // Rejected, and it does not say how many writes were applied.
        r.u64(2).u8(2).u8(0).u32(kNone).text("no");
        more(r, added.to_elements);
    });
    raw.element([&](Raw& r) {
        r.u64(3'000'000).u8(1).text("warm");
        more(r, added.to_elements);
    });
    if (values)
    {
        raw.u64(3'000'000).u64(4).u32(2).u32(0).u32(2).u8(0).u8(1).u8(2).f64(2.5);
    }
    more(raw, added.to_message);
    return raw;
}

void check_small_step(const wire::StepMessage& step, bool values)
{
    CHECK_EQ(step.session, std::uint64_t{11});
    CHECK(step.time == at(3ms));
    CHECK(!step.final);
    CHECK_EQ(step.results.size(), std::size_t{2});
    CHECK_EQ(step.results[0].seq, std::uint64_t{1});
    CHECK(step.results[0].status == CommandStatus::Applied);
    CHECK(step.results[0].late);
    CHECK(step.results[0].applied_at == std::optional<SimTime>(at(3ms)));
    CHECK(step.results[0].applied_writes == number(2));
    CHECK(step.results[0].reason.empty());
    CHECK_EQ(step.results[1].seq, std::uint64_t{2});
    CHECK(step.results[1].status == CommandStatus::Rejected);
    CHECK(!step.results[1].late);
    CHECK(!step.results[1].applied_at);
    CHECK(!step.results[1].applied_writes);
    CHECK(step.results[1].reason == "no");
    CHECK_EQ(step.events.size(), std::size_t{1});
    CHECK(step.events[0].time == at(3ms));
    CHECK(step.events[0].level == EventLevel::Warning);
    CHECK(step.events[0].text == "warm");
    CHECK_EQ(step.has_values, values);
    if (values)
    {
        CHECK(step.values.time == at(3ms));
        CHECK_EQ(step.values.sequence, std::uint64_t{4});
        CHECK_EQ(step.values.total, std::uint32_t{2});
        CHECK_EQ(step.values.first, std::uint32_t{0});
        CHECK(step.values.values == (std::vector<Value>{Value{true}, Value{2.5}}));
    }
}

bool same(const Command& a, const Command& b)
{
    return a.seq == b.seq && a.at == b.at && a.late == b.late && a.writes.size() == b.writes.size() &&
           std::equal(a.writes.begin(), a.writes.end(), b.writes.begin(),
                      [](const Write& x, const Write& y) { return x.name == y.name && x.value == y.value; });
}

/// The step messages of one step, as they were written.
std::vector<wire::Bytes> step_bytes(std::uint64_t session, SimTime time, bool final, const std::vector<CommandResult>& results,
                                    const std::vector<Event>& events, const Snapshot* sample, std::size_t max_bytes,
                                    const wire::AvailabilityView* availability = nullptr)
{
    std::vector<wire::Bytes> out;
    wire::Bytes scratch;
    wire::encode_step(session, time, final, results, events, sample, availability, max_bytes, scratch,
                      [&](const wire::Bytes& message) { out.push_back(message); });
    return out;
}

wire::Error decode_any(wire::Kind kind, std::span<const std::uint8_t> bytes)
{
    wire::Hello hello;
    wire::CommandMessage command;
    wire::SubmittedMessage submitted;
    wire::StepMessage step;
    switch (kind)
    {
    case wire::Kind::Hello:
        return wire::decode_hello(bytes, hello);
    case wire::Kind::Command:
        return wire::decode_command(bytes, command);
    case wire::Kind::Submitted:
        return wire::decode_submitted(bytes, submitted);
    default:
        return wire::decode_step(bytes, step);
    }
}

} // namespace

TEST_CASE(wire_v2_another_version_is_not_read)
{
    CHECK_EQ(wire::kVersion, std::uint16_t{2});
    // An older version and a newer one alike (and 0, which no build wrote): a mismatch that says
    // the version, and a hello still names its participant.
    for (const std::uint16_t version : {std::uint16_t{0}, std::uint16_t{1}, std::uint16_t{3}})
    {
        wire::Bytes hello = raw_hello(0, {}).bytes;
        hello[4] = static_cast<std::uint8_t>(version);
        wire::Hello decoded;
        const wire::Error error = wire::decode_hello(hello, decoded);
        CHECK(error.code == wire::ErrorCode::VersionMismatch);
        CHECK_EQ(error.version, version);
        CHECK(decoded.participant == "Engine");
        CHECK(error.text.find("written in version " + std::to_string(version) + " of the format; this side reads version 2") !=
              std::string::npos);

        wire::Bytes command = raw_command(0, {}).bytes;
        command[4] = static_cast<std::uint8_t>(version);
        CHECK(decode_any(wire::Kind::Command, command).code == wire::ErrorCode::VersionMismatch);
        CHECK(!wire::addressed(command).has_value());
        wire::Error peeked;
        const auto header = wire::peek(command, peeked);
        CHECK(header.has_value()); // readable: another version is no error here
        CHECK_EQ(header->version, version);
        CHECK_EQ(header->revision, std::uint8_t{0});
        for (const wire::Kind kind : {wire::Kind::Probe, wire::Kind::Ready, wire::Kind::LastWordReceived})
        {
            wire::Bytes session = Raw(kind, wire::kRevision).u64(1).bytes;
            session[4] = static_cast<std::uint8_t>(version);
            CHECK(!wire::addressed(session).has_value());
        }
    }
}

TEST_CASE(wire_v2_messages_round_trip_and_say_their_version)
{
    // The hand-written messages are what the encoder writes: this pins the layout of version 2.
    const wire::Hello hello = small_hello();
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK(bytes == raw_hello(wire::kRevision, {}).bytes);
    CHECK_EQ(bytes[kRevisionAt], wire::kRevision);
    wire::Hello decoded_hello;
    wire::Error error = wire::decode_hello(bytes, decoded_hello);
    CHECK(error.text.empty());
    CHECK_EQ(error.version, std::uint16_t{2});
    CHECK_EQ(error.revision, wire::kRevision);
    CHECK(decoded_hello == hello);
    wire::Error peeked;
    const auto header = wire::peek(bytes, peeked);
    CHECK(header.has_value());
    CHECK_EQ(header->version, std::uint16_t{2});
    CHECK_EQ(header->revision, wire::kRevision);

    const wire::CommandMessage command = small_command();
    CHECK(wire::encode_command(command, bytes));
    CHECK(bytes == raw_command(wire::kRevision, {}).bytes);
    wire::CommandMessage decoded_command;
    error = wire::decode_command(bytes, decoded_command);
    CHECK(error.text.empty());
    CHECK_EQ(error.version, std::uint16_t{2});
    CHECK_EQ(decoded_command.session, command.session);
    CHECK(same(decoded_command.command, command.command));
    const auto to = wire::addressed(bytes);
    CHECK(to.has_value());
    CHECK(to->kind == wire::Kind::Command);
    CHECK_EQ(to->session, command.session);
    CHECK_EQ(to->revision, wire::kRevision);

    // Submitted: the write the reason is about, or none.
    const wire::SubmittedMessage refused{9, 42, SubmitResult{SubmitStatus::Invalid, "gear: no", 1U}};
    wire::encode_submitted(refused, bytes);
    CHECK(bytes == raw_submitted(wire::kRevision, {}).bytes);
    wire::SubmittedMessage decoded_submitted;
    error = wire::decode_submitted(bytes, decoded_submitted);
    CHECK(error.text.empty());
    CHECK_EQ(error.version, std::uint16_t{2});
    CHECK_EQ(decoded_submitted.session, std::uint64_t{9});
    CHECK_EQ(decoded_submitted.seq, std::uint64_t{42});
    CHECK(decoded_submitted.result.status == SubmitStatus::Invalid);
    CHECK(decoded_submitted.result.reason == "gear: no");
    CHECK(decoded_submitted.result.failed_write == number(1));
    for (const auto& failed : {std::optional<std::uint32_t>{}, number(0), number(65535)})
    {
        decoded_submitted.result.failed_write = 7U;
        wire::encode_submitted(wire::SubmittedMessage{9, 42, SubmitResult{SubmitStatus::Invalid, "why", failed}}, bytes);
        CHECK(!wire::decode_submitted(bytes, decoded_submitted));
        CHECK(decoded_submitted.result.failed_write == failed);
    }

    // Step: how many writes of each command were applied.
    const std::vector<CommandResult> results{
        {1, CommandStatus::Applied, at(3ms), true, {}, 2U},
        {2, CommandStatus::Rejected, std::nullopt, false, "no", std::nullopt},
    };
    const std::vector<Event> events{{at(3ms), EventLevel::Warning, "warm"}};
    const Snapshot sample{at(3ms), 4, {Value{true}, Value{2.5}}};
    for (const bool values : {false, true})
    {
        const auto messages = step_bytes(11, at(3ms), false, results, events, values ? &sample : nullptr, wire::kMaxMessageBytes);
        CHECK_EQ(messages.size(), std::size_t{1});
        CHECK(messages[0] == raw_step(wire::kRevision, {}, values).bytes);
        wire::StepMessage step;
        error = wire::decode_step(messages[0], step);
        CHECK(error.text.empty());
        CHECK_EQ(error.version, std::uint16_t{2});
        check_small_step(step, values);
    }
    const std::vector<CommandResult> counted{
        {1, CommandStatus::Rejected, std::nullopt, false, "x: a setter threw", 0U},
        {2, CommandStatus::Applied, at(1ms), false, {}, 65535U},
        {3, CommandStatus::Late, std::nullopt, true, "its time had passed", 0U},
    };
    wire::StepMessage step;
    CHECK(!wire::decode_step(step_bytes(1, at(1ms), false, counted, {}, nullptr, wire::kMaxMessageBytes).at(0), step));
    CHECK_EQ(step.results.size(), counted.size());
    for (std::size_t i = 0; i < counted.size(); ++i)
    {
        CHECK(step.results[i].applied_writes == counted[i].applied_writes);
        CHECK(step.results[i].reason == counted[i].reason);
    }
    // Not said: empty, also in an object that had a number from a message before.
    std::vector<CommandResult> unsaid = counted;
    unsaid[0].applied_writes.reset();
    CHECK(!wire::decode_step(step_bytes(1, at(1ms), false, unsaid, {}, nullptr, wire::kMaxMessageBytes).at(0), step));
    CHECK(!step.results.at(0).applied_writes);

    // Probe, ready, last word received: the session and nothing else.
    for (const wire::Kind kind : {wire::Kind::Probe, wire::Kind::Ready, wire::Kind::LastWordReceived})
    {
        wire::encode_session(kind, 0xFEDCBA9876543210ULL, bytes);
        CHECK(bytes == Raw(kind, wire::kRevision).u64(0xFEDCBA9876543210ULL).bytes);
        const auto whom = wire::addressed(bytes);
        CHECK(whom.has_value());
        CHECK(whom->kind == kind);
        CHECK_EQ(whom->session, std::uint64_t{0xFEDCBA9876543210ULL});
        CHECK_EQ(whom->revision, wire::kRevision);
    }
}

TEST_CASE(wire_v2_what_a_newer_revision_added_is_skipped_and_from_an_equal_one_refused)
{
    const std::uint8_t newer = wire::kRevision + 1;
    // One kind of addition at a time, and all of them together.
    const Added additions[] = {
        {.to_elements = true}, {.to_message = true}, {.flags = true}, {.to_elements = true, .to_message = true, .flags = true}};
    for (const Added& added : additions)
    {
        for (const std::uint8_t revision : {newer, std::uint8_t{0xFF}})
        {
            wire::Hello hello;
            wire::Error error = wire::decode_hello(raw_hello(revision, added).bytes, hello);
            CHECK(error.text.empty());
            CHECK_EQ(error.revision, revision);
            CHECK(hello == small_hello());

            wire::CommandMessage command;
            error = wire::decode_command(raw_command(revision, added).bytes, command);
            CHECK(error.text.empty());
            CHECK_EQ(error.revision, revision);
            CHECK_EQ(command.session, std::uint64_t{77});
            CHECK(same(command.command, small_command().command));
            const auto to = wire::addressed(raw_command(revision, added).bytes);
            CHECK(to.has_value());
            CHECK_EQ(to->revision, revision);

            wire::SubmittedMessage submitted;
            error = wire::decode_submitted(raw_submitted(revision, added).bytes, submitted);
            CHECK(error.text.empty());
            CHECK_EQ(submitted.seq, std::uint64_t{42});
            CHECK(submitted.result.reason == "gear: no");
            CHECK(submitted.result.failed_write == number(1));

            for (const bool values : {false, true})
            {
                wire::StepMessage step;
                error = wire::decode_step(raw_step(revision, added, values).bytes, step);
                CHECK(error.text.empty());
                check_small_step(step, values);
            }
        }

        // The same bytes from a writer of this revision: it has nothing to add, so they are wrong.
        const bool hello_differs = added.to_elements || added.to_message || added.flags;
        const bool command_differs = added.to_elements || added.to_message; // a command has no flags
        const bool submitted_differs = added.to_message;                    // nor elements
        CHECK_EQ(decode_any(wire::Kind::Hello, raw_hello(wire::kRevision, added).bytes).code == wire::ErrorCode::Malformed, hello_differs);
        CHECK_EQ(decode_any(wire::Kind::Command, raw_command(wire::kRevision, added).bytes).code == wire::ErrorCode::Malformed,
                 command_differs);
        CHECK_EQ(decode_any(wire::Kind::Submitted, raw_submitted(wire::kRevision, added).bytes).code == wire::ErrorCode::Malformed,
                 submitted_differs);
        for (const bool values : {false, true})
        {
            CHECK(decode_any(wire::Kind::Step, raw_step(wire::kRevision, added, values).bytes).code == wire::ErrorCode::Malformed);
        }
    }
    // Nothing added: read from either.
    for (const std::uint8_t revision : {wire::kRevision, newer})
    {
        CHECK(!decode_any(wire::Kind::Hello, raw_hello(revision, {}).bytes));
        CHECK(!decode_any(wire::Kind::Command, raw_command(revision, {}).bytes));
        CHECK(!decode_any(wire::Kind::Submitted, raw_submitted(revision, {}).bytes));
        CHECK(!decode_any(wire::Kind::Step, raw_step(revision, {}, true).bytes));
    }

    // A probe, a ready and a last-word-received message that a newer revision made longer are still for their agent.
    for (const wire::Kind kind : {wire::Kind::Probe, wire::Kind::Ready, wire::Kind::LastWordReceived})
    {
        const auto to = wire::addressed(Raw(kind, newer).u64(4711).u32(1).bytes);
        CHECK(to.has_value());
        CHECK(to->kind == kind);
        CHECK_EQ(to->session, std::uint64_t{4711});
        CHECK_EQ(to->revision, newer);
        CHECK(wire::addressed(Raw(kind, newer).u64(4711).bytes).has_value());
        CHECK(!wire::addressed(Raw(kind, wire::kRevision).u64(4711).u32(1).bytes).has_value());
        CHECK(!wire::addressed(Raw(kind, newer).u32(4711).bytes).has_value()); // too short for a session
    }

    // What must not be skipped is refused from a newer revision too: an enumerator this build
    // does not know, a field of this revision that is not there, an element that reaches beyond
    // the message.
    Raw unknown_status(wire::Kind::Submitted, newer);
    unknown_status.u64(9).u64(42).u8(3).text("").u32(kNone);
    CHECK(decode_any(wire::Kind::Submitted, unknown_status.bytes).code == wire::ErrorCode::Malformed);
    Raw unknown_type(wire::Kind::Command, newer);
    unknown_type.u64(77).u64(5).u8(0).u64(0).u8(0).u32(1).element([](Raw& r) { r.text("on").u8(3).u64(0); });
    CHECK(decode_any(wire::Kind::Command, unknown_type.bytes).code == wire::ErrorCode::Malformed);
    Raw short_result(wire::Kind::Step, newer);
    short_result.u64(11).u64(0).u8(0).u16(1).u16(0).element([](Raw& r) { r.u64(1).u8(0).u8(0).u32(1); }); // no reason
    short_result.u64(0).u64(0).u64(0); // what follows the element does not make up for it
    CHECK(decode_any(wire::Kind::Step, short_result.bytes).code == wire::ErrorCode::Malformed);
    wire::Bytes cut = raw_step(newer, {.to_elements = true}, false).bytes;
    cut.pop_back();
    CHECK(decode_any(wire::Kind::Step, cut).code == wire::ErrorCode::Truncated);
}

TEST_CASE(wire_v2_the_size_of_an_element_is_checked)
{
    // The size of the first element of each kind of element, one byte off in both directions and
    // beyond the message.
    struct Case
    {
        wire::Kind kind;
        wire::Bytes bytes;
        std::size_t size_at; ///< of the first element
    };
    const std::size_t hello_head = 8 + (4 + 6) + 8 + 1 + 8 + 8 + 4 + 4;
    const std::size_t command_head = 8 + 8 + 8 + 1 + 8 + 1 + 4;
    const std::size_t step_head = 8 + 8 + 8 + 1 + 2 + 2;
    const wire::Bytes step = raw_step(wire::kRevision, {}, true).bytes;
    const std::uint32_t first_result = 8 + 1 + 1 + 8 + 4 + 4;
    const std::uint32_t second_result = 8 + 1 + 1 + 4 + 4 + 2;
    const Case cases[] = {
        {wire::Kind::Hello, raw_hello(wire::kRevision, {}).bytes, hello_head},
        {wire::Kind::Command, raw_command(wire::kRevision, {}).bytes, command_head},
        {wire::Kind::Step, step, step_head},                                         // a result
        {wire::Kind::Step, step, step_head + 4 + first_result + 4 + second_result},  // the event
    };
    for (const Case& c : cases)
    {
        CHECK(!decode_any(c.kind, c.bytes));
        CHECK(c.bytes[c.size_at] > 1 && c.bytes[c.size_at + 1] == 0);
        // Smaller than its content: the content does not fit the element.
        wire::Bytes smaller = c.bytes;
        --smaller[c.size_at];
        wire::Error error = decode_any(c.kind, smaller);
        CHECK(error.code == wire::ErrorCode::Malformed);
        CHECK(error.text.find("an element ends before its content does") != std::string::npos);
        // Larger: bytes of the element that this revision does not read.
        wire::Bytes larger = c.bytes;
        ++larger[c.size_at];
        error = decode_any(c.kind, larger);
        CHECK(error.code == wire::ErrorCode::Malformed);
        CHECK(error.text.find("after the end of") != std::string::npos);
        // Beyond the message: the message is cut short.
        wire::Bytes beyond = c.bytes;
        beyond[c.size_at + 2] = 1; // 65536 more
        error = decode_any(c.kind, beyond);
        CHECK(error.code == wire::ErrorCode::Truncated);
        CHECK(error.text.find("bytes with") != std::string::npos);
        // Exactly the rest of the message is not beyond it: the element then holds what follows
        // it, which this revision does not read.
        wire::Bytes rest = c.bytes;
        const auto left = static_cast<std::uint32_t>(c.bytes.size() - c.size_at - 4);
        for (std::size_t i = 0; i < 4; ++i)
        {
            rest[c.size_at + i] = static_cast<std::uint8_t>(left >> (8 * i));
        }
        CHECK(decode_any(c.kind, rest).code == wire::ErrorCode::Malformed);
    }

    // A text inside an element that reaches beyond the element but not beyond the message is
    // not read from the next element.
    Raw raw(wire::Kind::Command, wire::kRevision);
    raw.u64(77).u64(5).u8(0).u64(0).u8(0).u32(2);
    raw.element([](Raw& r) { r.u32(8).u8('o').u8('n').u8(0).u8(1); }); // a name of "8" bytes in an element of 8
    raw.element([](Raw& r) { r.text("gear").u8(0).u8(1); });
    CHECK(decode_any(wire::Kind::Command, raw.bytes).code == wire::ErrorCode::Malformed);
    // Enum items announced beyond their variable.
    Raw items(wire::Kind::Hello, wire::kRevision);
    items.text("Engine").u64(1).u8(0).u64(0).u64(0).u32(8).u32(2);
    items.element([](Raw& r) { r.text("gear").u8(3).u8(0).text("").u8(0).text("").u32(2).text("P").u64(0); });
    items.element([](Raw& r) { r.text("more").u8(3).u8(0).text("").u8(0).text("").u32(0); });
    CHECK(decode_any(wire::Kind::Hello, items.bytes).code == wire::ErrorCode::Malformed);
}

TEST_CASE(wire_v2_a_step_message_is_filled_to_the_byte)
{
    // What encode_step() counts for a result and an event is what it writes: results and events
    // that fill a message exactly go into one message of exactly that size, and one byte more
    // makes two, none of them beyond the limit.
    const std::size_t head = 8 + 8 + 8 + 1 + 2 + 2;
    const std::size_t result = 4 + 8 + 1 + 1 + 4 + 4; // without applied_at and reason
    const std::size_t event = 4 + 8 + 1 + 4;          // without text
    for (const std::size_t limit : {wire::kMinMessageBytes, std::size_t{3001}, wire::kMaxMessageBytes})
    {
        // Results: one with applied_at and a text of the largest size, and as many plain ones
        // as it takes; the last one takes what is left.
        std::vector<CommandResult> results;
        results.push_back({1, CommandStatus::Applied, at(1ms), false, std::string(wire::kMaxTextBytes, 'r'), 1U});
        std::size_t used = head + result + 8 + wire::kMaxTextBytes;
        CHECK(used <= wire::kMinMessageBytes); // the largest result alone fits the smallest message
        while (limit - used >= result)
        {
            const std::size_t reason = std::min<std::size_t>(limit - used - result, 700);
            // Not a rest that no result fits.
            const std::size_t take = limit - used - result - reason != 0 && limit - used - result - reason < result ? reason / 2 : reason;
            results.push_back({results.size() + 1, CommandStatus::Rejected, std::nullopt, true, std::string(take, 'x'), 0U});
            used += result + take;
        }
        CHECK_EQ(used, limit);
        auto messages = step_bytes(1, at(1ms), false, results, {}, nullptr, limit);
        CHECK_EQ(messages.size(), std::size_t{1});
        CHECK_EQ(messages[0].size(), limit);
        CHECK(!decode_any(wire::Kind::Step, messages[0]));
        results.back().reason += 'x';
        messages = step_bytes(1, at(1ms), false, results, {}, nullptr, limit);
        CHECK_EQ(messages.size(), std::size_t{2});
        CHECK_EQ(messages[0].size(), limit - result - results.back().reason.size() + 1);
        CHECK_EQ(messages[1].size(), head + result + results.back().reason.size());

        // Events, the same way.
        std::vector<Event> events;
        used = head;
        while (limit - used >= event)
        {
            const std::size_t text = std::min<std::size_t>(limit - used - event, wire::kMaxTextBytes);
            const std::size_t take = limit - used - event - text != 0 && limit - used - event - text < event ? text / 2 : text;
            events.push_back({at(1ms), EventLevel::Info, std::string(take, 'e')});
            used += event + take;
        }
        CHECK_EQ(used, limit);
        messages = step_bytes(1, at(1ms), false, {}, events, nullptr, limit);
        CHECK_EQ(messages.size(), std::size_t{1});
        CHECK_EQ(messages[0].size(), limit);
        CHECK(!decode_any(wire::Kind::Step, messages[0]));
        events.back().text.pop_back();
        events.back().text += "ee";
        messages = step_bytes(1, at(1ms), false, {}, events, nullptr, limit);
        CHECK_EQ(messages.size(), std::size_t{2});
        CHECK(messages[0].size() < limit);
        CHECK_EQ(messages[1].size(), head + event + events.back().text.size());
    }

    // The largest event alone fits the smallest message too.
    const std::vector<Event> largest{{at(1ms), EventLevel::Error, std::string(3 * wire::kMaxTextBytes, 'T')}};
    const auto one = step_bytes(1, at(1ms), false, {}, largest, nullptr, wire::kMinMessageBytes);
    CHECK_EQ(one.size(), std::size_t{1});
    CHECK_EQ(one[0].size(), head + event + wire::kMaxTextBytes);
}

TEST_CASE(wire_v2_the_smallest_elements_are_counted_right)
{
    // A decoder refuses a count that the rest of the message cannot hold, counting with the
    // smallest element there can be. Messages made of nothing but smallest elements are read:
    // the size it counts with is not larger than the smallest element.
    const std::size_t size = 4; // the size in front of an element
    wire::Hello hello;
    hello.participant = "E";
    hello.max_pending = 1;
    VariableInfo plain;
    plain.name = "v";
    hello.schema.assign(100, plain);
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(hello, bytes, why));
    // A variable: name, type, causality, unit, has, description, items, availability (revision 1).
    CHECK_EQ(bytes.size(), 8 + 5 + 8 + 1 + 8 + 8 + 4 + 4 + 100 * (size + 5 + 1 + 1 + 4 + 1 + 4 + 4 + 1));
    wire::Hello decoded_hello;
    CHECK(!wire::decode_hello(bytes, decoded_hello));
    CHECK(decoded_hello == hello);

    wire::CommandMessage command;
    command.command.writes.assign(100, Write{"", Value{true}});
    CHECK(wire::encode_command(command, bytes));
    CHECK_EQ(bytes.size(), 8 + 8 + 8 + 1 + 8 + 1 + 4 + 100 * (size + 4 + 2));
    wire::CommandMessage decoded_command;
    CHECK(!wire::decode_command(bytes, decoded_command));
    CHECK_EQ(decoded_command.command.writes.size(), std::size_t{100});

    const std::size_t result = 4 + 8 + 1 + 1 + 4 + 4;
    const std::size_t event = 4 + 8 + 1 + 4;
    const std::vector<CommandResult> results(100, CommandResult{1, CommandStatus::Late, std::nullopt, true, {}, 0U});
    const std::vector<Event> events(100, Event{at(1ms), EventLevel::Info, {}});
    wire::StepMessage step;
    auto messages = step_bytes(1, at(1ms), false, results, {}, nullptr, wire::kMaxMessageBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].size(), 8 + 8 + 8 + 1 + 2 + 2 + 100 * result);
    CHECK(!wire::decode_step(messages[0], step));
    CHECK_EQ(step.results.size(), std::size_t{100});
    messages = step_bytes(1, at(1ms), false, {}, events, nullptr, wire::kMaxMessageBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].size(), 8 + 8 + 8 + 1 + 2 + 2 + 100 * event);
    CHECK(!wire::decode_step(messages[0], step));
    CHECK_EQ(step.events.size(), std::size_t{100});

    // One more announced than the message holds: refused by the count, before any is read.
    const std::size_t results_at = 8 + 8 + 8 + 1;
    wire::Bytes more_results = step_bytes(1, at(1ms), false, results, {}, nullptr, wire::kMaxMessageBytes).at(0);
    ++more_results[results_at];
    wire::Error error = wire::decode_step(more_results, step);
    CHECK(error.code == wire::ErrorCode::Truncated);
    CHECK(error.text.find("more results or events announced") != std::string::npos);
    wire::Bytes more_events = messages[0];
    ++more_events[results_at + 2];
    error = wire::decode_step(more_events, step);
    CHECK(error.code == wire::ErrorCode::Truncated);
    CHECK(error.text.find("more results or events announced") != std::string::npos);
    CHECK(wire::encode_command(command, bytes));
    ++bytes[8 + 8 + 8 + 1 + 8 + 1];
    error = wire::decode_command(bytes, decoded_command);
    CHECK(error.code == wire::ErrorCode::Truncated);
    CHECK(error.text.find("writes announced") != std::string::npos);
    CHECK(wire::encode_hello(hello, bytes, why));
    ++bytes[8 + 5 + 8 + 1 + 8 + 8 + 4];
    error = wire::decode_hello(bytes, decoded_hello);
    CHECK(error.code == wire::ErrorCode::Truncated);
    CHECK(error.text.find("variables announced") != std::string::npos);
}

TEST_CASE(wire_v2_availability_from_an_older_and_from_a_newer_revision)
{
    // Revision 0 has no availability: its variables do not report (a hub takes them as Active),
    // and its step messages have no flag 4 (from it, the bit is refused like any unknown one).
    wire::Hello hello;
    CHECK(!wire::decode_hello(raw_hello(0, {}).bytes, hello));
    CHECK_EQ(hello.schema.size(), std::size_t{2});
    CHECK(!hello.schema[0].availability.has_value());
    CHECK(!hello.schema[1].availability.has_value());
    wire::StepMessage step;
    Raw old_flag(wire::Kind::Step, 0);
    old_flag.u64(11).u64(0).u8(4).u16(0).u16(0).u64(1).u32(1).u32(0).u32(1).u8(2);
    CHECK(wire::decode_step(old_flag.bytes, step).code == wire::ErrorCode::Malformed);

    // A newer revision: the availability is read, and what it added behind it (in the variable,
    // behind the part, and an unknown flag) is skipped.
    const std::uint8_t newer = wire::kRevision + 1;
    Raw newer_hello(wire::Kind::Hello, newer);
    newer_hello.text("Engine").u64(1).u8(0).u64(1'000'000).u64(10'000'000).u32(8).u32(1);
    newer_hello.element([](Raw& r) { r.text("on").u8(0).u8(0).text("").u8(0).text("").u32(0).u8(3).u32(0xDEADBEEF); });
    CHECK(!wire::decode_hello(newer_hello.bytes, hello));
    CHECK(hello.schema.at(0).availability == std::optional(Availability::Inactive));
    Raw newer_step(wire::Kind::Step, newer);
    newer_step.u64(11).u64(0).u8(static_cast<std::uint8_t>(4 | kUnknownFlag)).u16(0).u16(0);
    newer_step.u64(9).u32(3).u32(1).u32(2).u8(0).u8(2); // version 9, states 1 and 2 of 3
    newer_step.u8(0xAA).u32(0xDEADBEEF);                 // what the newer revision added
    CHECK(!wire::decode_step(newer_step.bytes, step));
    CHECK(step.has_availability);
    CHECK(!step.has_values);
    CHECK_EQ(step.availability.version, std::uint64_t{9});
    CHECK_EQ(step.availability.total, std::uint32_t{3});
    CHECK_EQ(step.availability.first, std::uint32_t{1});
    CHECK(step.availability.states == (std::vector<Availability>{Availability::Unknown, Availability::Inactive}));

    // What must not be skipped is refused: a state this build does not know, a part that reaches
    // beyond its list, a first byte of a variable beyond 3.
    for (const std::uint8_t revision : {wire::kRevision, newer})
    {
        Raw unknown_state(wire::Kind::Step, revision);
        unknown_state.u64(11).u64(0).u8(4).u16(0).u16(0).u64(1).u32(1).u32(0).u32(1).u8(3);
        CHECK(wire::decode_step(unknown_state.bytes, step).code == wire::ErrorCode::Malformed);
        Raw beyond(wire::Kind::Step, revision);
        beyond.u64(11).u64(0).u8(4).u16(0).u16(0).u64(1).u32(2).u32(1).u32(2).u8(1).u8(1);
        CHECK(wire::decode_step(beyond.bytes, step).code == wire::ErrorCode::Malformed);
        Raw bad_variable(wire::Kind::Hello, revision);
        bad_variable.text("Engine").u64(1).u8(0).u64(1'000'000).u64(10'000'000).u32(8).u32(1);
        bad_variable.element([](Raw& r) { r.text("on").u8(0).u8(0).text("").u8(0).text("").u32(0).u8(4); });
        CHECK(wire::decode_hello(bad_variable.bytes, hello).code == wire::ErrorCode::Malformed);
    }
    // From this revision, a variable without its last byte is short.
    Raw short_variable(wire::Kind::Hello, wire::kRevision);
    short_variable.text("Engine").u64(1).u8(0).u64(1'000'000).u64(10'000'000).u32(8).u32(1);
    short_variable.element([](Raw& r) { r.text("on").u8(0).u8(0).text("").u8(0).text("").u32(0); });
    CHECK(wire::decode_hello(short_variable.bytes, hello));
}

TEST_CASE(wire_v2_availability_is_split_like_the_values)
{
    // Behind the values, in as many parts as it takes; a message has at most one part of each,
    // and the parts of the list share its version.
    Snapshot sample{at(1ms), 2, {}};
    for (int i = 0; i < 300; ++i)
    {
        sample.values.emplace_back(std::int64_t{i});
    }
    std::vector<Availability> states;
    for (int i = 0; i < 5000; ++i)
    {
        states.push_back(static_cast<Availability>(i % 3));
    }
    const wire::AvailabilityView availability{42, states};
    for (const Snapshot* with : {static_cast<const Snapshot*>(nullptr), static_cast<const Snapshot*>(&sample)})
    {
        const auto messages = step_bytes(1, at(1ms), true, {}, {}, with, wire::kMinMessageBytes, &availability);
        CHECK(messages.size() >= 3);
        std::vector<Value> values;
        std::vector<Availability> read;
        bool values_done = with == nullptr;
        for (std::size_t i = 0; i < messages.size(); ++i)
        {
            CHECK(messages[i].size() <= wire::kMinMessageBytes);
            wire::StepMessage step;
            CHECK(!wire::decode_step(messages[i], step));
            CHECK_EQ(step.final, i + 1 == messages.size());
            if (step.has_values)
            {
                CHECK_EQ(step.values.first, static_cast<std::uint32_t>(values.size()));
                values.insert(values.end(), step.values.values.begin(), step.values.values.end());
                values_done = values.size() == sample.values.size();
            }
            if (step.has_availability)
            {
                CHECK(values_done); // the values first
                CHECK_EQ(step.availability.version, std::uint64_t{42});
                CHECK_EQ(step.availability.total, std::uint32_t{5000});
                CHECK_EQ(step.availability.first, static_cast<std::uint32_t>(read.size()));
                read.insert(read.end(), step.availability.states.begin(), step.availability.states.end());
            }
        }
        CHECK(read == states);
        if (with != nullptr)
        {
            CHECK(values == sample.values);
        }
    }
    // An empty list is one part of nothing.
    const wire::AvailabilityView none{3, {}};
    const auto empty = step_bytes(1, at(1ms), false, {}, {}, nullptr, wire::kMaxMessageBytes, &none);
    CHECK_EQ(empty.size(), std::size_t{1});
    wire::StepMessage step;
    CHECK(!wire::decode_step(empty.at(0), step));
    CHECK(step.has_availability);
    CHECK_EQ(step.availability.total, std::uint32_t{0});
    CHECK(step.availability.states.empty());
    // Nothing to say: no message.
    CHECK(step_bytes(1, at(1ms), false, {}, {}, nullptr, wire::kMaxMessageBytes).empty());
}
