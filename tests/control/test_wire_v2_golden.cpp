// SPDX-License-Identifier: MIT
//
// vehicle_sil::control::wire against the bytes of version 2, revision 0, written out by hand
// (wire_v2_golden.hpp): the encoder writes exactly them, and they decode to the structures they
// were written from. The base for whoever adds a field: a later revision still reads these bytes.
#include "test_harness.hpp"
#include "wire_v2_golden.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <cstdint>
#include <cstdio>
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
                        std::to_string(actual.size()) + " bytes, revision 0 has " + std::to_string(expected.size()) +
                        "; they differ from byte " + std::to_string(i) + " (from byte " + std::to_string(from) +
                        ": wrote " + hex(actual, from, 16) + "| revision 0 " + hex(expected, from, 16) +
                        "). A change of these bytes is a change of the format: ECUs built with revision 0 of "
                        "version 2 would no longer understand this encoder, nor it them. Add the field at the end "
                        "of its element or of the message, read and write it only for a peer of the new revision, "
                        "and count wire::kRevision up (wire.hpp)");
}

#define CHECK_BYTES(actual, expected) check_bytes((actual), (expected), #expected, __LINE__)

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
                                       const std::vector<Event>& events, const Snapshot* sample, std::size_t max_bytes)
{
    std::vector<wire::Bytes> out;
    wire::Bytes scratch;
    wire::encode_step(golden::kSession, time, final, results, events, sample, max_bytes, scratch,
                      [&](const wire::Bytes& message) { out.push_back(message); });
    return out;
}

/// Decoded without an error, as version 2 revision 0.
void check_read(const wire::Error& error, int line)
{
    if (error || error.version != 2 || error.revision != 0)
    {
        throw unit::Failure(std::string(__FILE__) + ":" + std::to_string(line) + ": not read as version 2 revision 0: version " +
                            std::to_string(error.version) + ", revision " + std::to_string(error.revision) + ", " + error.text);
    }
}

} // namespace

TEST_CASE(wire_v2_revision_0_hello_has_its_bytes)
{
    // What this file pins is revision 0. A later revision pins its own bytes beside these.
    CHECK_EQ(wire::kVersion, std::uint16_t{2});
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(golden::hello(), bytes, why));
    CHECK_BYTES(bytes, golden::kHello);

    wire::Hello decoded;
    check_read(wire::decode_hello(golden::kHello, decoded), __LINE__);
    CHECK(decoded == golden::hello());
}

TEST_CASE(wire_v2_revision_0_command_has_its_bytes)
{
    for (const auto& [message, expected] :
         {std::pair{golden::command_at(), std::span<const std::uint8_t>(golden::kCommandAt)},
          std::pair{golden::command_next_step(), std::span<const std::uint8_t>(golden::kCommandNextStep)}})
    {
        wire::Bytes bytes;
        CHECK(wire::encode_command(message, bytes));
        CHECK_BYTES(bytes, expected);
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
        CHECK_BYTES(bytes, expected);
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
    CHECK_BYTES(messages[0], golden::kStep);

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
    CHECK_BYTES(messages[0], first);
    CHECK_BYTES(messages[1], golden::kSplitSecond);

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
    CHECK_BYTES(messages[0], golden::kFinalStep);

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
        CHECK_BYTES(bytes, expected);
        const auto to = wire::addressed(expected);
        CHECK(to.has_value());
        CHECK(to->kind == kind);
        CHECK_EQ(to->session, golden::kSession);
        CHECK_EQ(to->revision, std::uint8_t{0});
        CHECK_EQ(to->revision, std::uint8_t{0});
    }
}
