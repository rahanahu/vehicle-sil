// SPDX-License-Identifier: MIT
//
// vehicle_sil::control::wire against the bytes of version 1 written out by hand
// (wire_v1_golden.hpp): the encoder writes exactly them, and they decode to the structures they
// were written from. ECUs built with version 1 send and expect these bytes; a change of the
// encoder that fails here breaks them.
#include "test_harness.hpp"
#include "wire_v1_golden.hpp"

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
namespace golden = vehicle_sil::control::wire_v1_golden;

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
/// around it, so that the field can be found in wire_v1_golden.hpp.
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
                        std::to_string(actual.size()) + " bytes, version 1 has " + std::to_string(expected.size()) +
                        "; they differ from byte " + std::to_string(i) + " (from byte " + std::to_string(from) +
                        ": wrote " + hex(actual, from, 16) + "| version 1 " + hex(expected, from, 16) +
                        "). A change of these bytes is a change of the format: ECUs that speak version 1 would no "
                        "longer understand this encoder, nor it them. Leave version 1 as it is and put the change "
                        "into a new version (kVersion)");
}

#define CHECK_BYTES(actual, expected) check_bytes((actual), (expected), #expected, __LINE__)

bool same(const Command& a, const Command& b)
{
    if (a.seq != b.seq || a.at != b.at || a.late != b.late || a.writes.size() != b.writes.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.writes.size(); ++i)
    {
        if (a.writes[i].name != b.writes[i].name || a.writes[i].value != b.writes[i].value)
        {
            return false;
        }
    }
    return true;
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
            a[i].late != b[i].late || a[i].reason != b[i].reason)
        {
            return false;
        }
    }
    return true;
}

bool same(const std::vector<Event>& a, const std::vector<Event>& b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (a[i].time != b[i].time || a[i].level != b[i].level || a[i].text != b[i].text)
        {
            return false;
        }
    }
    return true;
}

/// The messages encode_step() emits for one step.
std::vector<wire::Bytes> step_messages(SimTime time, bool final, const std::vector<CommandResult>& results,
                                       const std::vector<Event>& events, const Snapshot* sample, std::size_t max_bytes)
{
    std::vector<wire::Bytes> out;
    wire::Bytes scratch;
    wire::encode_step(golden::kSession, time, final, results, events, sample, max_bytes, scratch,
                      [&](const wire::Bytes& message) { out.push_back(message); });
    return out;
}

} // namespace

TEST_CASE(wire_v1_hello_has_the_bytes_of_version_1)
{
    CHECK_EQ(wire::kVersion, std::uint16_t{1});
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(golden::hello(), bytes, why));
    CHECK_BYTES(bytes, golden::kHello);

    wire::Hello decoded;
    const wire::Error error = wire::decode_hello(golden::kHello, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK(decoded == golden::hello());
}

TEST_CASE(wire_v1_command_has_the_bytes_of_version_1)
{
    wire::Bytes bytes;
    CHECK(wire::encode_command(golden::command_at(), bytes));
    CHECK_BYTES(bytes, golden::kCommandAt);
    CHECK(wire::encode_command(golden::command_next_step(), bytes));
    CHECK_BYTES(bytes, golden::kCommandNextStep);

    for (const auto& [message, expected] :
         {std::pair{golden::command_at(), std::span<const std::uint8_t>(golden::kCommandAt)},
          std::pair{golden::command_next_step(), std::span<const std::uint8_t>(golden::kCommandNextStep)}})
    {
        wire::CommandMessage decoded;
        const wire::Error error = wire::decode_command(expected, decoded);
        CHECK(error.text.empty());
        CHECK(!error);
        CHECK_EQ(decoded.session, golden::kSession);
        CHECK(same(decoded.command, message.command));
        const auto to = wire::addressed(expected);
        CHECK(to.has_value());
        CHECK(to->kind == wire::Kind::Command);
        CHECK_EQ(to->session, golden::kSession);
    }
}

TEST_CASE(wire_v1_submitted_has_the_bytes_of_version_1)
{
    wire::Bytes bytes;
    wire::encode_submitted(golden::submitted(), bytes);
    CHECK_BYTES(bytes, golden::kSubmitted);

    wire::SubmittedMessage decoded;
    const wire::Error error = wire::decode_submitted(golden::kSubmitted, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK_EQ(decoded.seq, golden::submitted().seq);
    CHECK(decoded.result.status == golden::submitted().result.status);
    CHECK(decoded.result.reason == golden::submitted().result.reason);
}

TEST_CASE(wire_v1_step_has_the_bytes_of_version_1)
{
    const Snapshot sample = golden::step_sample();
    const auto messages =
        step_messages(golden::step_time(), false, golden::step_results(), golden::step_events(), &sample, golden::kStepMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_BYTES(messages[0], golden::kStep);

    wire::StepMessage decoded;
    const wire::Error error = wire::decode_step(golden::kStep, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK(decoded.time == golden::step_time());
    CHECK(!decoded.final);
    CHECK(same(decoded.results, golden::step_results()));
    CHECK(same(decoded.events, golden::step_events()));
    CHECK(decoded.has_values);
    CHECK(decoded.values.time == sample.time);
    CHECK_EQ(decoded.values.sequence, sample.sequence);
    CHECK_EQ(decoded.values.total, std::uint32_t{3});
    CHECK_EQ(decoded.values.first, std::uint32_t{0});
    CHECK(decoded.values.values == sample.values);
}

TEST_CASE(wire_v1_split_step_has_the_bytes_of_version_1)
{
    const Snapshot sample = golden::split_sample();
    const auto messages = step_messages(golden::step_time(), false, {}, {}, &sample, golden::kSplitMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{2});

    // The first: its head, then the values 0 .. kSplitFirstCount - 1 as 0x01 and an i64 each.
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
    const wire::Error error = wire::decode_step(golden::kSplitSecond, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK(decoded.time == golden::step_time());
    CHECK(!decoded.final);
    CHECK(decoded.results.empty());
    CHECK(decoded.events.empty());
    CHECK(decoded.has_values);
    CHECK(decoded.values.time == sample.time);
    CHECK_EQ(decoded.values.sequence, sample.sequence);
    CHECK_EQ(decoded.values.total, golden::kSplitValues);
    CHECK_EQ(decoded.values.first, golden::kSplitFirstCount);
    CHECK(decoded.values.values == std::vector<Value>(sample.values.begin() + golden::kSplitFirstCount, sample.values.end()));
}

TEST_CASE(wire_v1_last_word_has_the_bytes_of_version_1)
{
    const auto messages = step_messages(golden::final_time(), true, golden::final_results(), {}, nullptr, golden::kStepMaxBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_BYTES(messages[0], golden::kFinalStep);

    wire::StepMessage decoded;
    const wire::Error error = wire::decode_step(golden::kFinalStep, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK_EQ(decoded.session, golden::kSession);
    CHECK(decoded.time == golden::final_time());
    CHECK(decoded.final);
    CHECK(same(decoded.results, golden::final_results()));
    CHECK(decoded.events.empty());
    CHECK(!decoded.has_values);
}

TEST_CASE(wire_v1_probe_ready_and_last_word_received_have_the_bytes_of_version_1)
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
    }
}
