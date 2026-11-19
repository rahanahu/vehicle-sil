// SPDX-License-Identifier: MIT
//
// vehicle_sil::control::wire against the bytes of version 2, written out by hand
// (wire_v2_golden.hpp): those of revision 0 decode to the structures they were written from, and
// the encoder writes them but for the revision byte (a hello of revision 1 has a byte more per
// variable); those of revision 1 the encoder writes exactly, and they decode as written. The base
// for whoever adds a field: a later revision still reads all of these bytes.
#include "test_harness.hpp"
#include "wire_v2_golden.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace vehicle_sil;
using namespace vehicle_sil::control;
namespace wire = vehicle_sil::control::wire;
namespace golden = vehicle_sil::control::wire_v2_golden;

std::string hex(std::span<const std::uint8_t> bytes, std::size_t from, std::size_t n)
{
    std::string out;
    for (std::size_t i = from; i < bytes.size() && i < from + n; ++i)
    {
        char b[4];
        std::snprintf(b, sizeof b, "%02X ", bytes[i]);
        out += b;
    }
    return out;
}

/// The bytes are the golden ones; a failure names the first byte that differs, with the bytes
/// around it, so that the field can be found in wire_v2_golden.hpp.
void check_bytes(std::span<const std::uint8_t> actual, std::span<const std::uint8_t> expected, const char* what, int line)
{
    std::size_t i = 0;
    while (i < actual.size() && i < expected.size() && actual[i] == expected[i])
    {
        ++i;
    }
    if (i == actual.size() && i == expected.size())
    {
        return;
    }
    const std::size_t from = i < 8 ? 0 : i - 8;
    throw unit::Failure(std::string(__FILE__) + ":" + std::to_string(line) + ": " + what + ": the encoder wrote " +
                        std::to_string(actual.size()) + " bytes, the pinned bytes are " + std::to_string(expected.size()) +
                        "; they differ from byte " + std::to_string(i) + " (from byte " + std::to_string(from) +
                        ": wrote " + hex(actual, from, 16) + "| pinned " + hex(expected, from, 16) +
                        "). A change of these bytes is a change of the format: ECUs built with an earlier revision of "
                        "version 2 would no longer understand this encoder, nor it them. Add the field at the end "
                        "of its element or of the message, read and write it only for a peer of the new revision, "
                        "and count wire::kRevision up (wire.hpp)");
}

#define CHECK_BYTES(actual, expected) check_bytes((actual), (expected), #expected, __LINE__)

/// Bytes of revision 0 that revision 1 added nothing to, as this encoder writes them: with its
/// revision in the header.
wire::Bytes as_written(std::span<const std::uint8_t> revision_0)
{
    wire::Bytes out(revision_0.begin(), revision_0.end());
    out.at(7) = wire::kRevision;
    return out;
}

bool same(const std::vector<CommandResult>& a, const std::vector<CommandResult>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].seq != b[i].seq || a[i].status != b[i].status || a[i].applied_at != b[i].applied_at ||
            a[i].late != b[i].late || a[i].reason != b[i].reason || a[i].applied_writes != b[i].applied_writes)
        {
            return false;
        }
    }
    return true;
}

std::vector<wire::Bytes> step_messages(SimTime time, bool final, const std::vector<CommandResult>& results,
                                       const std::vector<Event>& events, const Snapshot* sample, std::size_t max_bytes,
                                       const wire::AvailabilityView* availability = nullptr)
{
    std::vector<wire::Bytes> out;
    wire::Bytes scratch;
    wire::encode_step(golden::kSession, time, final, results, events, sample, availability, max_bytes, scratch,
                      [&](const wire::Bytes& message) { out.push_back(message); });
    return out;
}

/// Decoded without an error, as version 2 of `revision`.
void check_read(const wire::Error& error, int line, std::uint8_t revision = 0)
{
    if (error || error.version != 2 || error.revision != revision)
    {
        throw unit::Failure(std::string(__FILE__) + ":" + std::to_string(line) + ": not read as version 2 revision " +
                            std::to_string(revision) + ": version " + std::to_string(error.version) + ", revision " +
                            std::to_string(error.revision) + ", " + error.text);
    }
}

} // namespace

TEST_CASE(wire_v2_revision_0_hello_has_its_bytes)
{
    // Revision 1 added a byte to each variable (pinned in wire_v2_revision_1_hello_has_its_bytes):
    // these bytes are read, and none of their variables reports its availability.
    CHECK_EQ(wire::kVersion, std::uint16_t{2});
    wire::Hello decoded;
    check_read(wire::decode_hello(golden::kHello, decoded), __LINE__);
    CHECK(decoded == golden::hello());
    for (const auto& variable : decoded.schema)
    {
        CHECK(!variable.availability.has_value());
    }
}

TEST_CASE(wire_v2_revision_0_command_has_its_bytes)
{
    for (const auto& [message, expected] :
         {std::pair{golden::command_at(), std::span<const std::uint8_t>(golden::kCommandAt)},
          std::pair{golden::command_next_step(), std::span<const std::uint8_t>(golden::kCommandNextStep)}})
    {
        wire::Bytes bytes;
        CHECK(wire::encode_command(message, bytes));
        CHECK_BYTES(bytes, as_written(expected));
        wire::CommandMessage decoded;
        check_read(wire::decode_command(expected, decoded), __LINE__);
        CHECK_EQ(decoded.session, golden::kSession);
        CHECK_EQ(decoded.command.seq, message.command.seq);
        CHECK(decoded.command.at == message.command.at);
        CHECK(decoded.command.late == message.command.late);
        CHECK_EQ(decoded.command.writes.size(), message.command.writes.size());
        for (std::size_t i = 0; i < message.command.writes.size(); ++i)
        {
            CHECK(decoded.command.writes[i].name == message.command.writes[i].name);
            CHECK(decoded.command.writes[i].value == message.command.writes[i].value);
        }
        const auto to = wire::addressed(expected);
        CHECK(to.has_value());
        CHECK(to->kind == wire::Kind::Command);
        CHECK_EQ(to->session, golden::kSession);
        CHECK_EQ(to->revision, std::uint8_t{0});
    }
}

TEST_CASE(wire_v2_revision_0_submitted_has_its_bytes)
{
    for (const auto& [message, expected] :
         {std::pair{golden::submitted(), std::span<const std::uint8_t>(golden::kSubmitted)},
          std::pair{golden::submitted_without_a_write(), std::span<const std::uint8_t>(golden::kSubmittedWithoutAWrite)}})
    {
        wire::Bytes bytes;
        wire::encode_submitted(message, bytes);
        CHECK_BYTES(bytes, as_written(expected));
        wire::SubmittedMessage decoded;
        check_read(wire::decode_submitted(expected, decoded), __LINE__);
        CHECK_EQ(decoded.session, golden::kSession);
        CHECK_EQ(decoded.seq, message.seq);
        CHECK(decoded.result.status == message.result.status);
        CHECK(decoded.result.reason == message.result.reason);
        CHECK(decoded.result.failed_write == message.result.failed_write);
    }
    CHECK(golden::submitted().result.failed_write == std::optional<std::uint32_t>{0});
    CHECK(!golden::submitted_without_a_write().result.failed_write.has_value());
}

TEST_CASE(wire_v2_revision_0_step_has_its_bytes)
{
    const Snapshot sample = golden::step_sample();
    const auto messages =
        step_messages(golden::step_time(), false, golden::step_results(), golden::step_events(), &sample, golden::kStepMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_BYTES(messages[0], as_written(golden::kStep));

    wire::StepMessage decoded;
    check_read(wire::decode_step(golden::kStep, decoded), __LINE__);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK(decoded.time == golden::step_time());
    CHECK(!decoded.final);
    CHECK(same(decoded.results, golden::step_results()));
    CHECK(decoded.results.at(0).applied_writes == std::optional<std::uint32_t>{3});
    CHECK(decoded.results.at(1).applied_writes == std::optional<std::uint32_t>{1});
    CHECK_EQ(decoded.events.size(), std::size_t{1});
    CHECK(decoded.events[0].time == golden::step_events()[0].time);
    CHECK(decoded.events[0].level == golden::step_events()[0].level);
    CHECK(decoded.events[0].text == golden::step_events()[0].text);
    CHECK(decoded.has_values);
    CHECK(decoded.values.time == sample.time);
    CHECK_EQ(decoded.values.sequence, sample.sequence);
    CHECK_EQ(decoded.values.total, std::uint32_t{3});
    CHECK_EQ(decoded.values.first, std::uint32_t{0});
    CHECK(decoded.values.values == sample.values);
    CHECK(!decoded.has_availability);
}

TEST_CASE(wire_v2_revision_0_split_step_has_its_bytes)
{
    const Snapshot sample = golden::split_sample();
    const auto messages = step_messages(golden::step_time(), false, {}, {}, &sample, golden::kSplitMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{2});
    wire::Bytes first(std::begin(golden::kSplitFirstHead), std::end(golden::kSplitFirstHead));
    for (std::uint32_t i = 0; i < golden::kSplitFirstCount; ++i)
    {
        first.push_back(0x01);
        for (int b = 0; b < 8; ++b)
        {
            first.push_back(static_cast<std::uint8_t>(std::uint64_t{i} >> (8 * b)));
        }
    }
    CHECK_EQ(first.size(), std::size_t{2046});
    CHECK_BYTES(messages[0], as_written(first));
    CHECK_BYTES(messages[1], as_written(golden::kSplitSecond));

    wire::StepMessage decoded;
    check_read(wire::decode_step(golden::kSplitSecond, decoded), __LINE__);
    CHECK_EQ(decoded.values.total, golden::kSplitValues);
    CHECK_EQ(decoded.values.first, golden::kSplitFirstCount);
    CHECK(decoded.values.values == std::vector<Value>(sample.values.begin() + golden::kSplitFirstCount, sample.values.end()));
}

TEST_CASE(wire_v2_revision_0_last_word_has_its_bytes)
{
    const auto messages = step_messages(golden::final_time(), true, golden::final_results(), {}, nullptr, golden::kStepMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_BYTES(messages[0], as_written(golden::kFinalStep));

    wire::StepMessage decoded;
    check_read(wire::decode_step(golden::kFinalStep, decoded), __LINE__);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK(decoded.time == golden::final_time());
    CHECK(decoded.final);
    CHECK(same(decoded.results, golden::final_results()));
    CHECK(!decoded.results.at(0).applied_writes.has_value());
    CHECK(decoded.events.empty());
    CHECK(!decoded.has_values);
}

TEST_CASE(wire_v2_revision_0_probe_ready_and_last_word_received_have_their_bytes)
{
    const std::pair<wire::Kind, std::span<const std::uint8_t>> cases[] = {
        {wire::Kind::Probe, golden::kProbe},
        {wire::Kind::Ready, golden::kReady},
        {wire::Kind::LastWordReceived, golden::kLastWordReceived},
    };
    for (const auto& [kind, expected] : cases)
    {
        wire::Bytes bytes;
        wire::encode_session(kind, golden::kSession, bytes);
        CHECK_BYTES(bytes, as_written(expected));
        const auto to = wire::addressed(expected);
        CHECK(to.has_value());
        CHECK(to->kind == kind);
        CHECK_EQ(to->session, golden::kSession);
        CHECK_EQ(to->revision, std::uint8_t{0});
    }
}

TEST_CASE(wire_v2_revision_1_hello_has_its_bytes)
{
    CHECK_EQ(wire::kRevision, std::uint8_t{1});
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(golden::hello_revision_1(), bytes, why));
    CHECK_BYTES(bytes, golden::kHelloRevision1);

    wire::Hello decoded;
    check_read(wire::decode_hello(golden::kHelloRevision1, decoded), __LINE__, 1);
    CHECK(decoded == golden::hello_revision_1());
    CHECK(decoded.schema.at(0).availability == std::optional(Availability::Unknown));
    CHECK(decoded.schema.at(1).availability == std::optional(Availability::Inactive));
    CHECK(decoded.schema.at(2).availability == std::optional(Availability::Active));
    CHECK(!decoded.schema.at(3).availability.has_value());
}

namespace {

/// kHello as the encoder of revision 1 writes it: the revision in the header, and each variable
/// one byte longer, its availability (`availability[i]` for variable i) behind its enum items.
wire::Bytes hello_with_availability(std::span<const std::uint8_t> revision_0, const std::vector<std::uint8_t>& availability)
{
    const auto u32_at = [&](std::size_t at) {
        return static_cast<std::uint32_t>(revision_0[at]) | static_cast<std::uint32_t>(revision_0[at + 1]) << 8 |
               static_cast<std::uint32_t>(revision_0[at + 2]) << 16 | static_cast<std::uint32_t>(revision_0[at + 3]) << 24;
    };
    // Header 8, participant, session 8, virtual time 1, step 8, sample period 8, max pending 4.
    std::size_t at = 8 + 4 + u32_at(8) + 8 + 1 + 8 + 8 + 4;
    const std::uint32_t variables = u32_at(at);
    at += 4;
    wire::Bytes out(revision_0.begin(), revision_0.begin() + static_cast<std::ptrdiff_t>(at));
    out.at(7) = std::uint8_t{1};
    for (std::uint32_t i = 0; i < variables; ++i)
    {
        const std::uint32_t size = u32_at(at) + 1;
        out.insert(out.end(), {static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                               static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24)});
        out.insert(out.end(), revision_0.begin() + static_cast<std::ptrdiff_t>(at + 4),
                   revision_0.begin() + static_cast<std::ptrdiff_t>(at + size + 3));
        out.push_back(availability.at(i));
        at += size + 3;
    }
    CHECK_EQ(at, revision_0.size());
    return out;
}

} // namespace

TEST_CASE(wire_v2_revision_1_hello_keeps_every_field_of_revision_0)
{
    // The variables of kHello (every field: unit, minimum, maximum, step, description, enum
    // items) as revision 1 writes them: the bytes of revision 0, and the availability behind.
    CHECK_EQ(wire::kRevision, std::uint8_t{1});
    wire::Hello hello = golden::hello();
    hello.schema.at(0).availability = Availability::Inactive; // byte 3
    hello.schema.at(1).availability = Availability::Unknown;  // byte 1
    hello.schema.at(3).availability = Availability::Active;   // byte 2; "gain" does not report (0)
    const wire::Bytes expected = hello_with_availability(golden::kHello, {3, 1, 0, 2});
    CHECK_EQ(expected.size(), sizeof golden::kHello + hello.schema.size());
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK_BYTES(bytes, expected);

    wire::Hello decoded;
    check_read(wire::decode_hello(expected, decoded), __LINE__, 1);
    CHECK(decoded == hello);
}

TEST_CASE(wire_v2_revision_1_step_has_its_bytes)
{
    const Snapshot sample = golden::step_sample_revision_1();
    const auto states = golden::step_availability();
    const wire::AvailabilityView availability{golden::kAvailabilityVersion, states};
    const auto messages = step_messages(golden::step_time(), false, {}, {}, &sample, golden::kStepMaxBytes, &availability);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_BYTES(messages[0], golden::kStepRevision1);

    wire::StepMessage decoded;
    check_read(wire::decode_step(golden::kStepRevision1, decoded), __LINE__, 1);
    CHECK(decoded.has_values);
    CHECK(decoded.values.values == sample.values);
    CHECK(decoded.has_availability);
    CHECK_EQ(decoded.availability.version, golden::kAvailabilityVersion);
    CHECK_EQ(decoded.availability.total, std::uint32_t{4});
    CHECK_EQ(decoded.availability.first, std::uint32_t{0});
    CHECK(decoded.availability.states == states);

    // The availability alone is said too (a step without values, results and events).
    const auto alone = step_messages(golden::step_time(), false, {}, {}, nullptr, golden::kStepMaxBytes, &availability);
    CHECK_EQ(alone.size(), std::size_t{1});
    CHECK_BYTES(alone[0], golden::kStepAvailabilityOnly);
    check_read(wire::decode_step(golden::kStepAvailabilityOnly, decoded), __LINE__, 1);
    CHECK(!decoded.has_values);
    CHECK(!decoded.final);
    CHECK(decoded.has_availability);
    CHECK(decoded.availability.states == states);
}
