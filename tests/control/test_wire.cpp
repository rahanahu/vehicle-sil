// SPDX-License-Identifier: MIT
//
// vehicle_sil::control::wire: the messages between an agent and a hub as bytes. Every message
// round trips; a decoder refuses what is cut, corrupted, of another version or random, without
// reading outside the bytes (the sanitizer presets check that); what a step has to say is split
// at the size limit and put together again.
#include "test_harness.hpp"

#include "vehicle_sil/control/wire.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::control;
namespace wire = vehicle_sil::control::wire;
using vehicle_sil::unit::throws;

SimTime at(SimDuration d)
{
    return SimTime{} + d;
}

wire::Hello sample_hello()
{
    wire::Hello hello;
    hello.participant = "Engine";
    hello.session = 0x0123456789ABCDEFULL;
    hello.virtual_time = true;
    hello.step = 1ms;
    hello.sample_period = 10ms;
    hello.max_pending = 256;
    VariableInfo speed;
    speed.name = "engine.speed";
    speed.type = ValueType::Real;
    speed.causality = Causality::Output;
    speed.unit = "rpm";
    speed.min = 0.0;
    speed.max = 8031.875;
    speed.step = 0.125;
    speed.description = "the engine speed \xE5\x9B\x9E\xE8\xBB\xA2"; // UTF-8
    VariableInfo gear;
    gear.name = "gear";
    gear.type = ValueType::Enum;
    gear.causality = Causality::Input;
    gear.items = {{"P", 0}, {"R", 1}, {"D", -3}};
    VariableInfo gain;
    gain.name = "gain";
    gain.type = ValueType::Integer;
    gain.causality = Causality::Parameter;
    gain.max = 10.0;
    VariableInfo on;
    on.name = "on";
    on.type = ValueType::Bool;
    hello.schema = {speed, gear, gain, on};
    return hello;
}

wire::Bytes encoded(const wire::Hello& hello)
{
    wire::Bytes bytes;
    std::string why;
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK(why.empty());
    return bytes;
}

wire::CommandMessage sample_command()
{
    wire::CommandMessage m;
    m.session = 77;
    m.command.seq = 1'700'000'000'000'123ULL;
    m.command.at = at(1500us);
    m.command.late = LatePolicy::ApplyNext;
    m.command.writes = {{"on", Value{true}},
                        {"gear", Value{std::int64_t{-3}}},
                        {"engine.speed", Value{-0.0}},
                        {"big", Value{std::numeric_limits<std::int64_t>::lowest()}}};
    return m;
}

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

/// The step messages of one step, decoded again.
std::vector<wire::StepMessage> step_messages(std::uint64_t session, SimTime time, bool final,
                                             const std::vector<CommandResult>& results, const std::vector<Event>& events,
                                             const Snapshot* sample, std::size_t max_bytes, std::size_t* largest = nullptr)
{
    std::vector<wire::StepMessage> out;
    wire::Bytes scratch;
    wire::encode_step(session, time, final, results, events, sample, max_bytes, scratch, [&](const wire::Bytes& message) {
        CHECK(message.size() <= max_bytes);
        if (largest != nullptr)
        {
            *largest = std::max(*largest, message.size());
        }
        wire::StepMessage decoded;
        const wire::Error error = wire::decode_step(message, decoded);
        CHECK(error.text.empty());
        out.push_back(std::move(decoded));
    });
    return out;
}

/// The messages put together as a hub's transport does.
struct Assembled
{
    std::vector<CommandResult> results;
    std::vector<Event> events;
    std::vector<Snapshot> samples;
    std::size_t finals = 0;
};

Assembled assemble(const std::vector<wire::StepMessage>& messages)
{
    Assembled out;
    Snapshot partial;
    for (std::size_t i = 0; i < messages.size(); ++i)
    {
        const auto& m = messages[i];
        out.results.insert(out.results.end(), m.results.begin(), m.results.end());
        out.events.insert(out.events.end(), m.events.begin(), m.events.end());
        if (m.final)
        {
            ++out.finals;
            CHECK_EQ(i, messages.size() - 1);
        }
        if (m.has_values)
        {
            if (m.values.first == 0)
            {
                partial = Snapshot{m.values.time, m.values.sequence, {}};
            }
            CHECK_EQ(m.values.sequence, partial.sequence);
            CHECK_EQ(static_cast<std::size_t>(m.values.first), partial.values.size());
            partial.values.insert(partial.values.end(), m.values.values.begin(), m.values.values.end());
            if (partial.values.size() == m.values.total)
            {
                out.samples.push_back(partial);
            }
        }
    }
    return out;
}

bool same(const CommandResult& a, const CommandResult& b)
{
    return a.seq == b.seq && a.status == b.status && a.applied_at == b.applied_at && a.late == b.late && a.reason == b.reason;
}

} // namespace

TEST_CASE(wire_hello_round_trips)
{
    const wire::Hello hello = sample_hello();
    const wire::Bytes bytes = encoded(hello);
    wire::Hello decoded;
    const wire::Error error = wire::decode_hello(bytes, decoded);
    CHECK(error.text.empty());
    CHECK(!error);
    CHECK(decoded == hello);

    wire::Error peeked;
    const auto header = wire::peek(bytes, peeked);
    CHECK(header.has_value());
    CHECK(header->kind == wire::Kind::Hello);
    CHECK_EQ(header->version, wire::kVersion);

    // Without variables, in real time.
    wire::Hello bare;
    bare.participant = "B";
    bare.max_pending = 1;
    CHECK(!wire::decode_hello(encoded(bare), decoded));
    CHECK(decoded == bare);
}

TEST_CASE(wire_hello_that_cannot_be_announced_is_refused_when_encoded)
{
    wire::Bytes bytes{1, 2, 3};
    std::string why;
    wire::Hello hello = sample_hello();
    hello.participant.clear();
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(bytes.empty());
    CHECK(why.find("participant") != std::string::npos);

    hello = sample_hello();
    hello.max_pending = 0;
    CHECK(!wire::encode_hello(hello, bytes, why));
    hello.max_pending = wire::kMaxPending + 1;
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(why.find("max_pending") != std::string::npos);
    hello.max_pending = wire::kMaxPending;
    CHECK(wire::encode_hello(hello, bytes, why));

    // A schema whose description is larger than a hello may be.
    hello = sample_hello();
    hello.schema.assign(5, hello.schema.front());
    for (auto& v : hello.schema)
    {
        v.description.assign(wire::kMaxHelloBytes / 4, 'x');
    }
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(bytes.empty());
    CHECK(why.find("shorten the descriptions") != std::string::npos);
    hello.schema.resize(3);
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK(bytes.size() <= wire::kMaxHelloBytes);
    wire::Hello decoded;
    CHECK(!wire::decode_hello(bytes, decoded));
    CHECK(decoded == hello);
}

TEST_CASE(wire_hello_of_another_version_names_its_participant)
{
    wire::Bytes bytes = encoded(sample_hello());
    // The version is the two bytes after the four of the magic.
    bytes[4] = static_cast<std::uint8_t>(wire::kVersion + 1);
    // Whatever a later version puts behind the participant.
    bytes.resize(8 + 4 + 6);
    bytes.push_back(0xFF);
    wire::Hello decoded;
    const wire::Error error = wire::decode_hello(bytes, decoded);
    CHECK(error.code == wire::ErrorCode::VersionMismatch);
    CHECK_EQ(error.version, static_cast<std::uint16_t>(wire::kVersion + 1));
    CHECK(decoded.participant == "Engine");
    CHECK(error.text.find("version 2") != std::string::npos);

    // Too short for the participant: still a mismatch, without a name.
    bytes.resize(8 + 4 + 2);
    const wire::Error cut = wire::decode_hello(bytes, decoded);
    CHECK(cut.code == wire::ErrorCode::VersionMismatch);
    CHECK(decoded.participant.empty());

    // The other kinds say the version too.
    wire::Bytes command;
    CHECK(wire::encode_command(sample_command(), command));
    command[5] = 1; // version 257
    wire::CommandMessage m;
    const wire::Error other = wire::decode_command(command, m);
    CHECK(other.code == wire::ErrorCode::VersionMismatch);
    CHECK_EQ(other.version, static_cast<std::uint16_t>(wire::kVersion + 256));
    wire::Error peeked;
    CHECK(wire::peek(command, peeked).has_value()); // readable: another version is no error here
    CHECK_EQ(peeked.version, static_cast<std::uint16_t>(wire::kVersion + 256));
}

TEST_CASE(wire_command_and_submitted_round_trip)
{
    const wire::CommandMessage command = sample_command();
    wire::Bytes bytes;
    CHECK(wire::encode_command(command, bytes));
    wire::CommandMessage decoded;
    CHECK(!wire::decode_command(bytes, decoded));
    CHECK_EQ(decoded.session, command.session);
    CHECK(same(decoded.command, command.command));
    // -0.0 keeps its sign: the value is carried as its bits.
    CHECK(std::signbit(std::get<double>(decoded.command.writes[2].value)));

    wire::CommandMessage next;
    next.session = 1;
    next.command.seq = 2;
    next.command.writes = {{"x", Value{std::numeric_limits<double>::quiet_NaN()}}};
    CHECK(wire::encode_command(next, bytes));
    CHECK(!wire::decode_command(bytes, decoded));
    CHECK(!decoded.command.at.has_value());
    CHECK(decoded.command.late == LatePolicy::Reject);
    CHECK(std::isnan(std::get<double>(decoded.command.writes[0].value)));

    // A command is not split: one that does not fit is not encoded.
    wire::CommandMessage huge;
    huge.command.writes.assign(wire::kMaxMessageBytes / 16, Write{"a.variable.name", Value{1.0}});
    CHECK(!wire::encode_command(huge, bytes));
    CHECK(bytes.empty());

    for (const SubmitStatus status : {SubmitStatus::Queued, SubmitStatus::Busy, SubmitStatus::Invalid})
    {
        wire::SubmittedMessage submitted{9, 42, SubmitResult{status, status == SubmitStatus::Queued ? "" : "why"}};
        wire::encode_submitted(submitted, bytes);
        wire::SubmittedMessage back;
        CHECK(!wire::decode_submitted(bytes, back));
        CHECK_EQ(back.session, std::uint64_t{9});
        CHECK_EQ(back.seq, std::uint64_t{42});
        CHECK(back.result.status == status);
        CHECK(back.result.reason == submitted.result.reason);
    }
    // A long reason is cut, not inside a character.
    std::string reason(wire::kMaxTextBytes - 1, 'r');
    reason += "\xE3\x81\x82\xE3\x81\x84"; // two characters of three bytes
    wire::encode_submitted(wire::SubmittedMessage{1, 1, SubmitResult{SubmitStatus::Invalid, reason}}, bytes);
    wire::SubmittedMessage back;
    CHECK(!wire::decode_submitted(bytes, back));
    CHECK(back.result.reason == std::string(wire::kMaxTextBytes - 1, 'r'));
}

TEST_CASE(wire_decoding_as_another_kind_is_refused)
{
    wire::Bytes bytes;
    CHECK(wire::encode_command(sample_command(), bytes));
    wire::StepMessage step;
    CHECK(wire::decode_step(bytes, step).code == wire::ErrorCode::WrongKind);
    wire::Hello hello;
    CHECK(wire::decode_hello(bytes, hello).code == wire::ErrorCode::WrongKind);
    wire::SubmittedMessage submitted;
    CHECK(wire::decode_submitted(bytes, submitted).code == wire::ErrorCode::WrongKind);

    wire::Error error;
    const wire::Bytes text{'h', 'e', 'l', 'l', 'o', ' ', 'w', 'o', 'r', 'l', 'd'};
    CHECK(!wire::peek(text, error).has_value());
    CHECK(error.code == wire::ErrorCode::NotAMessage);
    wire::CommandMessage command;
    CHECK(wire::decode_command(text, command).code == wire::ErrorCode::NotAMessage);
    CHECK(wire::decode_command({}, command).code == wire::ErrorCode::Truncated);
    bytes[6] = 99; // an unknown kind
    CHECK(!wire::peek(bytes, error).has_value());
    CHECK(error.code == wire::ErrorCode::WrongKind);
}

TEST_CASE(wire_step_message_round_trips)
{
    const std::vector<CommandResult> results{
        {1, CommandStatus::Applied, at(3ms), false, {}},
        {2, CommandStatus::Late, std::nullopt, true, "its time had passed"},
        {3, CommandStatus::Rejected, std::nullopt, false, "a setter threw: no"},
        {4, CommandStatus::Cancelled, std::nullopt, false, "the simulation ended"},
        {5, CommandStatus::Applied, at(0ms), true, {}},
    };
    const std::vector<Event> events{{at(3ms), EventLevel::Info, "one"}, {at(3ms), EventLevel::Error, ""}, {at(2ms), EventLevel::Warning, "w"}};
    const Snapshot sample{at(3ms), 4, {Value{true}, Value{std::int64_t{-7}}, Value{2.5}, Value{false}}};

    const auto messages = step_messages(11, at(3ms), false, results, events, &sample, wire::kMaxMessageBytes);
    CHECK_EQ(messages.size(), std::size_t{1});
    CHECK_EQ(messages[0].session, std::uint64_t{11});
    CHECK(messages[0].time == at(3ms));
    CHECK(!messages[0].final);
    const Assembled all = assemble(messages);
    CHECK_EQ(all.results.size(), results.size());
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        CHECK(same(all.results[i], results[i]));
    }
    CHECK_EQ(all.events.size(), events.size());
    for (std::size_t i = 0; i < events.size(); ++i)
    {
        CHECK(all.events[i].time == events[i].time);
        CHECK(all.events[i].level == events[i].level);
        CHECK(all.events[i].text == events[i].text);
    }
    CHECK_EQ(all.samples.size(), std::size_t{1});
    CHECK(all.samples[0].time == sample.time);
    CHECK_EQ(all.samples[0].sequence, sample.sequence);
    CHECK(all.samples[0].values == sample.values);

    // Nothing to say: no message. The last word is a message even then.
    CHECK(step_messages(11, at(4ms), false, {}, {}, nullptr, wire::kMaxMessageBytes).empty());
    const auto last = step_messages(11, at(4ms), true, {}, {}, nullptr, wire::kMaxMessageBytes);
    CHECK_EQ(last.size(), std::size_t{1});
    CHECK(last[0].final);
    CHECK(!last[0].has_values);
    // A sample of an agent without variables is a sample.
    const Snapshot empty{at(5ms), 9, {}};
    const auto none = step_messages(11, at(5ms), false, {}, {}, &empty, wire::kMaxMessageBytes);
    CHECK_EQ(none.size(), std::size_t{1});
    CHECK_EQ(assemble(none).samples.size(), std::size_t{1});

    wire::Bytes scratch;
    CHECK(throws<std::invalid_argument>([&] {
        wire::encode_step(1, at(0ms), false, {}, {}, nullptr, wire::kMinMessageBytes - 1, scratch, [](const wire::Bytes&) {});
    }));
    CHECK(throws<std::invalid_argument>([&] {
        wire::encode_step(1, at(0ms), false, {}, {}, nullptr, wire::kMaxMessageBytes + 1, scratch, [](const wire::Bytes&) {});
    }));
}

TEST_CASE(wire_step_beyond_the_size_limit_is_split_and_put_together_again)
{
    std::mt19937_64 rng(20261112);
    std::vector<CommandResult> results;
    for (std::uint64_t i = 0; i < 300; ++i)
    {
        results.push_back({i, static_cast<CommandStatus>(i % 4), i % 4 == 0 ? std::optional<SimTime>(at(1ms * i)) : std::nullopt,
                           i % 3 == 0, std::string(i % 97, 'r')});
    }
    // One reason and one text beyond the limit of a text: cut, and still within a message.
    results[7].reason.assign(3 * wire::kMaxTextBytes, 'L');
    std::vector<Event> events;
    for (int i = 0; i < 200; ++i)
    {
        events.push_back({at(1ms * i), static_cast<EventLevel>(i % 3), std::string(static_cast<std::size_t>(i) % 131, 'e')});
    }
    events[5].text.assign(2 * wire::kMaxTextBytes, 'T');
    Snapshot sample{at(42ms), 17, {}};
    for (int i = 0; i < 5000; ++i)
    {
        switch (rng() % 3)
        {
        case 0:
            sample.values.emplace_back(rng() % 2 == 0);
            break;
        case 1:
            sample.values.emplace_back(static_cast<std::int64_t>(rng()));
            break;
        default:
            sample.values.emplace_back(static_cast<double>(rng()) / 7.0);
            break;
        }
    }

    for (const std::size_t limit : {wire::kMinMessageBytes, std::size_t{4096}, std::size_t{10000}, wire::kMaxMessageBytes})
    {
        std::size_t largest = 0;
        const auto messages = step_messages(5, at(42ms), true, results, events, &sample, limit, &largest);
        CHECK(messages.size() > 1);
        CHECK(largest <= limit);
        // The messages are filled: nothing like one item per message.
        CHECK(messages.size() <= (results.size() * 120 + events.size() * 150 + sample.values.size() * 9) / (limit / 2) + 3);
        const Assembled all = assemble(messages);
        CHECK_EQ(all.finals, std::size_t{1});
        CHECK_EQ(all.results.size(), results.size());
        for (std::size_t i = 0; i < results.size(); ++i)
        {
            CommandResult expected = results[i];
            if (expected.reason.size() > wire::kMaxTextBytes)
            {
                expected.reason.resize(wire::kMaxTextBytes);
            }
            CHECK(same(all.results[i], expected));
        }
        CHECK_EQ(all.events.size(), events.size());
        for (std::size_t i = 0; i < events.size(); ++i)
        {
            CHECK(all.events[i].text == events[i].text.substr(0, wire::kMaxTextBytes));
        }
        CHECK_EQ(all.samples.size(), std::size_t{1});
        CHECK(all.samples[0].values == sample.values);
        CHECK_EQ(all.samples[0].sequence, sample.sequence);
    }
}

TEST_CASE(wire_truncated_messages_are_refused_at_every_length)
{
    std::vector<wire::Bytes> messages;
    messages.push_back(encoded(sample_hello()));
    messages.emplace_back();
    CHECK(wire::encode_command(sample_command(), messages.back()));
    messages.emplace_back();
    wire::encode_submitted(wire::SubmittedMessage{3, 4, SubmitResult{SubmitStatus::Busy, "too many"}}, messages.back());
    const Snapshot sample{at(1ms), 1, {Value{true}, Value{std::int64_t{5}}, Value{1.5}}};
    wire::Bytes scratch;
    wire::encode_step(1, at(1ms), true, std::vector<CommandResult>{{1, CommandStatus::Applied, at(1ms), false, "r"}},
                      std::vector<Event>{{at(1ms), EventLevel::Info, "event"}}, &sample, wire::kMaxMessageBytes, scratch,
                      [&](const wire::Bytes& m) { messages.push_back(m); });
    CHECK_EQ(messages.size(), std::size_t{4});

    const auto decode_any = [](std::span<const std::uint8_t> bytes, std::size_t kind) {
        wire::Hello hello;
        wire::CommandMessage command;
        wire::SubmittedMessage submitted;
        wire::StepMessage step;
        switch (kind)
        {
        case 0:
            return wire::decode_hello(bytes, hello);
        case 1:
            return wire::decode_command(bytes, command);
        case 2:
            return wire::decode_submitted(bytes, submitted);
        default:
            return wire::decode_step(bytes, step);
        }
    };
    for (std::size_t kind = 0; kind < messages.size(); ++kind)
    {
        const wire::Bytes& whole = messages[kind];
        CHECK(!decode_any(whole, kind));
        for (std::size_t n = 0; n < whole.size(); ++n)
        {
            // A copy of exactly n bytes: a read beyond it is a read outside the allocation.
            const wire::Bytes part(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(n));
            const wire::Error error = decode_any(part, kind);
            CHECK(error.code == wire::ErrorCode::Truncated);
            CHECK(!error.text.empty());
        }
        // Bytes after the end are refused as well.
        wire::Bytes longer = whole;
        longer.push_back(0);
        CHECK(decode_any(longer, kind).code == wire::ErrorCode::Malformed);
    }
}

TEST_CASE(wire_corrupted_lengths_and_enumerators_are_refused)
{
    // Every byte of every message set to every of a few values: the decoder answers (an error or
    // not), reads nothing outside the message and allocates nothing a length field asks for
    // beyond what the message can hold.
    std::vector<wire::Bytes> messages;
    messages.push_back(encoded(sample_hello()));
    messages.emplace_back();
    CHECK(wire::encode_command(sample_command(), messages.back()));
    const Snapshot sample{at(1ms), 1, {Value{true}, Value{std::int64_t{5}}, Value{1.5}}};
    wire::Bytes scratch;
    wire::encode_step(1, at(1ms), false, std::vector<CommandResult>{{1, CommandStatus::Applied, at(1ms), false, "r"}},
                      std::vector<Event>{{at(1ms), EventLevel::Info, "event"}}, &sample, wire::kMaxMessageBytes, scratch,
                      [&](const wire::Bytes& m) { messages.push_back(m); });
    std::size_t refused = 0;
    std::size_t tried = 0;
    for (std::size_t kind = 0; kind < messages.size(); ++kind)
    {
        for (std::size_t i = 0; i < messages[kind].size(); ++i)
        {
            for (const std::uint8_t value : {std::uint8_t{0x00}, std::uint8_t{0x7F}, std::uint8_t{0x80}, std::uint8_t{0xFF}})
            {
                wire::Bytes bytes = messages[kind];
                if (bytes[i] == value)
                {
                    continue;
                }
                bytes[i] = value;
                wire::Hello hello;
                wire::CommandMessage command;
                wire::StepMessage step;
                const wire::Error error = kind == 0   ? wire::decode_hello(bytes, hello)
                                          : kind == 1 ? wire::decode_command(bytes, command)
                                                      : wire::decode_step(bytes, step);
                ++tried;
                refused += error ? 1 : 0;
            }
        }
    }
    // Most corruptions are seen (a changed digit of a number or letter of a name is not).
    CHECK(tried > 1000);
    CHECK(refused > tried / 4);

    // A count that the message cannot hold, in each place that has one.
    wire::Bytes hello_bytes = encoded(sample_hello());
    const std::size_t schema_count = 8 + 4 + 6 + 8 + 1 + 8 + 8 + 4;
    for (std::size_t i = 0; i < 4; ++i)
    {
        hello_bytes[schema_count + i] = 0xFF;
    }
    wire::Hello hello;
    const wire::Error many = wire::decode_hello(hello_bytes, hello);
    CHECK(many.code == wire::ErrorCode::Truncated);
    CHECK(many.text.find("variables announced") != std::string::npos);

    // A hello larger than a hello may be is refused by its size alone.
    wire::Bytes large = encoded(sample_hello());
    large.resize(wire::kMaxHelloBytes + 1);
    CHECK(wire::decode_hello(large, hello).code == wire::ErrorCode::TooLarge);
    wire::Bytes command;
    CHECK(wire::encode_command(sample_command(), command));
    command.resize(wire::kMaxMessageBytes + 1);
    wire::CommandMessage m;
    CHECK(wire::decode_command(command, m).code == wire::ErrorCode::TooLarge);

    // A part of a sample beyond the sample's end.
    wire::StepMessage step;
    wire::Bytes part;
    const Snapshot two{at(0ms), 1, {Value{true}, Value{false}}};
    wire::encode_step(1, at(0ms), false, {}, {}, &two, wire::kMaxMessageBytes, scratch, [&](const wire::Bytes& b) { part = b; });
    const std::size_t total_at = 8 + 8 + 8 + 1 + 2 + 2 + 8 + 8;
    part[total_at] = 1; // total 1, with 2 values in the part
    CHECK(wire::decode_step(part, step).code == wire::ErrorCode::Malformed);
}

TEST_CASE(wire_random_bytes_never_crash_a_decoder)
{
    std::mt19937_64 rng(4242);
    const wire::Bytes valid_hello = encoded(sample_hello());
    wire::Bytes valid_command;
    CHECK(wire::encode_command(sample_command(), valid_command));
    std::size_t accepted = 0;
    for (int round = 0; round < 20000; ++round)
    {
        wire::Bytes bytes;
        const std::size_t size = rng() % 200;
        if (round % 4 == 0)
        {
            // Pure noise.
            bytes.resize(size);
            for (auto& b : bytes)
            {
                b = static_cast<std::uint8_t>(rng());
            }
        }
        else
        {
            // A valid message with a few random bytes changed, cut at a random length: gets past
            // the header, which pure noise almost never does.
            bytes = round % 2 == 0 ? valid_hello : valid_command;
            bytes[6] = static_cast<std::uint8_t>(1 + rng() % 4);
            for (int i = 0; i < 3; ++i)
            {
                bytes[8 + rng() % (bytes.size() - 8)] = static_cast<std::uint8_t>(rng());
            }
            bytes.resize(8 + rng() % (bytes.size() - 7));
        }
        // Exactly sized: a read outside is a read outside the allocation.
        const wire::Bytes exact(bytes.begin(), bytes.end());
        wire::Hello hello;
        wire::CommandMessage command;
        wire::SubmittedMessage submitted;
        wire::StepMessage step;
        wire::Error error;
        static_cast<void>(wire::peek(exact, error));
        accepted += wire::decode_hello(exact, hello) ? 0 : 1;
        accepted += wire::decode_command(exact, command) ? 0 : 1;
        accepted += wire::decode_submitted(exact, submitted) ? 0 : 1;
        accepted += wire::decode_step(exact, step) ? 0 : 1;
    }
    // Some of the changed messages are still messages; the test is that nothing crashed.
    CHECK(accepted < 20000);
}

TEST_CASE(wire_hello_is_bounded_in_what_it_makes_a_reader_keep)
{
    wire::Bytes bytes;
    std::string why;
    wire::Hello decoded;
    // Names: 1 to kMaxNameBytes bytes, the participant's and each variable's.
    wire::Hello hello = sample_hello();
    hello.participant.assign(wire::kMaxNameBytes, 'p');
    hello.schema[0].name.assign(wire::kMaxNameBytes, 'v');
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK(!wire::decode_hello(bytes, decoded));
    CHECK(decoded == hello);
    hello.participant += 'p';
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(why.find("participant name has 257 bytes") != std::string::npos);
    hello = sample_hello();
    hello.schema[1].name.clear();
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(why.find("name of a variable") != std::string::npos);
    hello.schema[1].name.assign(wire::kMaxNameBytes + 1, 'v');
    CHECK(!wire::encode_hello(hello, bytes, why));
    // As many variables as a hello may have, and one more.
    hello = sample_hello();
    VariableInfo plain;
    plain.name = "v";
    hello.schema.assign(wire::kMaxVariables, plain);
    CHECK(wire::encode_hello(hello, bytes, why));
    CHECK(!wire::decode_hello(bytes, decoded));
    CHECK_EQ(decoded.schema.size(), wire::kMaxVariables);
    hello.schema.push_back(plain);
    CHECK(!wire::encode_hello(hello, bytes, why));
    CHECK(why.find("at most 65536") != std::string::npos);

    // What an encoder refuses, a decoder refuses too (built by hand: the encoder will not).
    const auto raw_hello = [](std::uint16_t version, const std::string& participant, std::uint32_t variables, std::size_t size) {
        wire::Bytes raw{'V', 'S', 'C', 'W', static_cast<std::uint8_t>(version), static_cast<std::uint8_t>(version >> 8), 1, 0};
        const auto u32 = [&raw](std::uint32_t v) {
            for (int i = 0; i < 4; ++i)
            {
                raw.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
            }
        };
        u32(static_cast<std::uint32_t>(participant.size()));
        raw.insert(raw.end(), participant.begin(), participant.end());
        raw.resize(raw.size() + 8 + 1 + 8 + 8, 0); // session, virtual time, step, sample period
        u32(256);                                  // max_pending
        u32(variables);
        raw.resize(std::max(raw.size(), size), 0); // the rest: zeros
        return raw;
    };
    // The largest hello, filled with variables that are all zeros (an empty name, no items):
    // refused at the first, and nothing was kept for the number announced.
    const wire::Error zeros = wire::decode_hello(raw_hello(wire::kVersion, "Flood", 200000, wire::kMaxHelloBytes), decoded);
    CHECK(zeros.code == wire::ErrorCode::TooLarge);
    CHECK(decoded.schema.empty());
    const wire::Error empty_name = wire::decode_hello(raw_hello(wire::kVersion, "Flood", 1000, wire::kMaxHelloBytes), decoded);
    CHECK(empty_name.code == wire::ErrorCode::Malformed);
    CHECK(empty_name.text.find("a variable name of 0 bytes") != std::string::npos);
    CHECK(decoded.schema.size() <= 1);
    CHECK(wire::decode_hello(raw_hello(wire::kVersion, "", 0, 0), decoded).code == wire::ErrorCode::Malformed);
    CHECK(wire::decode_hello(raw_hello(wire::kVersion, std::string(wire::kMaxNameBytes + 1, 'n'), 0, 0), decoded).code
          == wire::ErrorCode::Malformed);

    // A hello of another version: not read beyond the size limit either, and a name that is
    // too long is not handed on.
    const wire::Error large = wire::decode_hello(raw_hello(wire::kVersion + 1, "Newer", 0, wire::kMaxHelloBytes + 1), decoded);
    CHECK(large.code == wire::ErrorCode::TooLarge);
    CHECK(decoded.participant.empty());
    const wire::Error other = wire::decode_hello(raw_hello(wire::kVersion + 1, "Newer", 0, wire::kMaxHelloBytes), decoded);
    CHECK(other.code == wire::ErrorCode::VersionMismatch);
    CHECK(decoded.participant == "Newer");
    const wire::Error long_name =
        wire::decode_hello(raw_hello(wire::kVersion + 1, std::string(wire::kMaxNameBytes + 1, 'n'), 0, 0), decoded);
    CHECK(long_name.code == wire::ErrorCode::VersionMismatch);
    CHECK(decoded.participant.empty());
}

TEST_CASE(wire_probe_and_ready_and_whom_a_message_is_for)
{
    wire::Bytes bytes;
    for (const wire::Kind kind : {wire::Kind::Probe, wire::Kind::Ready})
    {
        wire::encode_session(kind, 0xFEDCBA9876543210ULL, bytes);
        CHECK_EQ(bytes.size(), std::size_t{16});
        const auto to = wire::addressed(bytes);
        CHECK(to.has_value());
        CHECK(to->kind == kind);
        CHECK_EQ(to->session, std::uint64_t{0xFEDCBA9876543210ULL});
        // Its session and nothing else; of this version.
        wire::Bytes longer = bytes;
        longer.push_back(0);
        CHECK(!wire::addressed(longer).has_value());
        for (std::size_t n = 0; n < bytes.size(); ++n)
        {
            CHECK(!wire::addressed(wire::Bytes(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(n))).has_value());
        }
        wire::Bytes other = bytes;
        other[4] = static_cast<std::uint8_t>(wire::kVersion + 1);
        CHECK(!wire::addressed(other).has_value());
        wire::Error error;
        CHECK(wire::peek(bytes, error).has_value());
    }
    CHECK(throws<std::invalid_argument>([&] { wire::encode_session(wire::Kind::Command, 1, bytes); }));

    // A command says whom it is for before it is decoded; the other kinds are for nobody.
    CHECK(wire::encode_command(sample_command(), bytes));
    const auto to = wire::addressed(bytes);
    CHECK(to.has_value());
    CHECK(to->kind == wire::Kind::Command);
    CHECK_EQ(to->session, sample_command().session);
    CHECK(!wire::addressed(encoded(sample_hello())).has_value());
    wire::encode_submitted(wire::SubmittedMessage{3, 4, SubmitResult{}}, bytes);
    CHECK(!wire::addressed(bytes).has_value());
    CHECK(!wire::addressed({}).has_value());
    wire::Bytes large(wire::kMaxMessageBytes + 1, 0);
    CHECK(!wire::addressed(large).has_value());
}

TEST_CASE(wire_a_decoder_given_the_same_object_again_starts_clean)
{
    // The transports keep one message object per kind: nothing of the message before stays.
    wire::StepMessage step;
    const Snapshot sample{at(1ms), 1, {Value{true}, Value{std::int64_t{5}}}};
    wire::Bytes scratch;
    wire::Bytes full;
    wire::encode_step(1, at(1ms), false, std::vector<CommandResult>{{1, CommandStatus::Applied, at(1ms), true, "reason"}},
                      std::vector<Event>{{at(1ms), EventLevel::Info, "event"}}, &sample, wire::kMaxMessageBytes, scratch,
                      [&](const wire::Bytes& m) { full = m; });
    wire::Bytes last;
    wire::encode_step(2, at(2ms), true, {}, {}, nullptr, wire::kMaxMessageBytes, scratch, [&](const wire::Bytes& m) { last = m; });
    CHECK(!wire::decode_step(full, step));
    CHECK(step.has_values);
    CHECK(!wire::decode_step(last, step));
    CHECK_EQ(step.session, std::uint64_t{2});
    CHECK(step.final);
    CHECK(step.results.empty());
    CHECK(step.events.empty());
    CHECK(!step.has_values);
    CHECK(step.values.values.empty());

    wire::CommandMessage command;
    wire::Bytes bytes;
    CHECK(wire::encode_command(sample_command(), bytes));
    CHECK(!wire::decode_command(bytes, command));
    wire::CommandMessage plain;
    plain.command.seq = 9;
    plain.command.writes = {{"x", Value{false}}};
    CHECK(wire::encode_command(plain, bytes));
    CHECK(!wire::decode_command(bytes, command));
    CHECK(same(command.command, plain.command));
    CHECK(!command.command.at.has_value());

    wire::Hello hello;
    CHECK(!wire::decode_hello(encoded(sample_hello()), hello));
    wire::Hello bare;
    bare.participant = "B";
    CHECK(!wire::decode_hello(encoded(bare), hello));
    CHECK(hello == bare);
}
