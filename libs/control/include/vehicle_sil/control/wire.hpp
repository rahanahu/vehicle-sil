// SPDX-License-Identifier: MIT
//
// The messages between a control agent and a hub as bytes, for a transport that carries opaque
// messages (vehicle_sil::control_silkit carries them over SIL Kit's publish/subscribe). No
// dependency: an ECU needs nothing but this library to speak it (vehicle_sil::control).
#pragma once

#include "vehicle_sil/control/agent.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace vehicle_sil::control::wire {

/// The version of the format. A decoder accepts only its own version; every message says which
/// version wrote it, and a hello of any version can be read far enough to name its participant
/// (decode_hello()), so that a mismatch is reported instead of looking like silence.
inline constexpr std::uint16_t kVersion = 1;

/// The largest command, submitted and step message. What an agent has to say in a step (results,
/// events, the values) is split into as many step messages as it takes (encode_step()); a command
/// is not split (its writes take effect together): one that does not fit is not sent.
inline constexpr std::size_t kMaxMessageBytes = 64 * 1024;
/// The smallest limit encode_step() takes (every single result and event fits it).
inline constexpr std::size_t kMinMessageBytes = 2048;
/// The largest hello. A hello is one message (a late subscriber gets only the last message of a
/// publisher): a schema that does not fit is refused when it is encoded.
inline constexpr std::size_t kMaxHelloBytes = 4 * 1024 * 1024;
/// A reason or an event text beyond this many bytes is cut (at a UTF-8 character) when encoded.
inline constexpr std::size_t kMaxTextBytes = 1024;
/// The largest AgentOptions::max_pending a hello may announce.
inline constexpr std::size_t kMaxPending = 65536;
/// The most variables a hello may describe.
inline constexpr std::size_t kMaxVariables = 65536;
/// The longest participant name and variable name of a hello, in bytes (and at least one).
inline constexpr std::size_t kMaxNameBytes = 256;

using Bytes = std::vector<std::uint8_t>;

enum class Kind : std::uint8_t
{
    Hello = 1,     ///< agent -> hub, once, kept for late subscribers
    Command = 2,   ///< hub -> agent
    Submitted = 3, ///< agent -> hub: what Agent::submit() returned
    Step = 4,      ///< agent -> hub: what a step produced, or the agent's last word
    Probe = 5,     ///< hub -> agent: "do you receive my commands?" (sent again until answered)
    Ready = 6,     ///< agent -> hub: the answer to a probe: commands sent from now on arrive
};

enum class ErrorCode
{
    Ok,
    Truncated,       ///< the message ends before its content does
    NotAMessage,     ///< it does not begin like a message of this format
    VersionMismatch, ///< written by another version (Error::version)
    WrongKind,       ///< a message of another kind than asked for (or of none)
    TooLarge,        ///< larger than the limit for its kind, or it announces more than it can hold
    Malformed,       ///< a value out of range, an unknown enumerator, or bytes after the end
};

struct Error
{
    ErrorCode code = ErrorCode::Ok;
    std::uint16_t version = 0; ///< the version the message says it has, once it could be read
    std::string text;          ///< what is wrong, for a log
    explicit operator bool() const noexcept { return code != ErrorCode::Ok; }
};

/// What an agent says about itself (control::PeerHello, and what the hub's transport needs).
struct Hello
{
    std::string participant;       ///< not empty
    std::uint64_t session = 0;     ///< new for every transport; every other message carries it
    bool virtual_time = false;     ///< the agent's steps run in the time synchronisation
    SimDuration step{};            ///< its step (virtual time), else 0
    SimDuration sample_period{};   ///< AgentOptions::sample_period
    std::size_t max_pending = 256; ///< AgentOptions::max_pending, 1..kMaxPending
    std::vector<VariableInfo> schema;

    friend bool operator==(const Hello&, const Hello&) = default;
};

struct CommandMessage
{
    std::uint64_t session = 0; ///< the agent it is for
    Command command;
};

struct SubmittedMessage
{
    std::uint64_t session = 0;
    std::uint64_t seq = 0;
    SubmitResult result;
};

/// A part of a sample: `values` are the variables first, first + 1, ... of `total`.
struct ValuesPart
{
    SimTime time{};
    std::uint64_t sequence = 0;
    std::uint32_t total = 0;
    std::uint32_t first = 0;
    std::vector<Value> values;
};

struct StepMessage
{
    std::uint64_t session = 0;
    SimTime time{};     ///< the start of the step that produced it (the agent's last step for the last word)
    bool final = false; ///< the agent's last word: nothing follows from this session
    std::vector<CommandResult> results;
    std::vector<Event> events;
    bool has_values = false;
    ValuesPart values; ///< with has_values (not an optional: a decoder that is given the same message object again reuses its storage)
};

/// The kind and version of a message, without decoding it. An Error for bytes that do not begin
/// like a message; a message of another version is not an error here.
struct Header
{
    std::uint16_t version = 0;
    Kind kind = Kind::Hello;
};
[[nodiscard]] std::optional<Header> peek(std::span<const std::uint8_t> bytes, Error& error);

/// Each encoder replaces `out`.
/// false (`out` empty, `why` says what to do): the hello would exceed kMaxHelloBytes, its
/// participant name or a variable's name is empty or longer than kMaxNameBytes, it has more than
/// kMaxVariables variables, or max_pending is not within 1..kMaxPending.
[[nodiscard]] bool encode_hello(const Hello& hello, Bytes& out, std::string& why);
/// false (`out` empty): the command would exceed kMaxMessageBytes.
[[nodiscard]] bool encode_command(const CommandMessage& message, Bytes& out);
void encode_submitted(const SubmittedMessage& message, Bytes& out);
/// A probe for, or the ready answer of, the agent `session` (`kind`: Kind::Probe or Kind::Ready).
void encode_session(Kind kind, std::uint64_t session, Bytes& out);
/// What a step at `time` produced, as step messages of at most `max_bytes` each (kMinMessageBytes
/// ..kMaxMessageBytes; std::invalid_argument otherwise), given to `emit` in order: the results,
/// then the events, then `sample` (null: none) in as many parts as it takes. Nothing is emitted
/// for nothing to say, unless `final` (the last message then has StepMessage::final). `scratch`
/// is the buffer the messages are built in (kept by the caller, so that a step allocates nothing
/// once it has grown).
void encode_step(std::uint64_t session, SimTime time, bool final, std::span<const CommandResult> results,
                 std::span<const Event> events, const Snapshot* sample, std::size_t max_bytes, Bytes& scratch,
                 const std::function<void(const Bytes& message)>& emit);

/// The decoders never throw and never read outside `bytes`; `out` is unspecified after an error.
/// A decoder that is given the same `out` again reuses its storage. The size limit of the kind is
/// looked at before the version: a message of another version is not read beyond it either.
/// A hello of another version: ErrorCode::VersionMismatch with Error::version, and
/// `out.participant` is set if it could be read and is not longer than kMaxNameBytes (the
/// beginning of a hello, up to the participant, is the same in every version).
[[nodiscard]] Error decode_hello(std::span<const std::uint8_t> bytes, Hello& out);
[[nodiscard]] Error decode_command(std::span<const std::uint8_t> bytes, CommandMessage& out);
[[nodiscard]] Error decode_submitted(std::span<const std::uint8_t> bytes, SubmittedMessage& out);
[[nodiscard]] Error decode_step(std::span<const std::uint8_t> bytes, StepMessage& out);
/// The session of a command, probe or ready message of this version, without decoding the rest
/// (an agent looks at it first: every agent receives every command). Empty for anything else.
struct Addressed
{
    Kind kind = Kind::Command;
    std::uint64_t session = 0;
};
[[nodiscard]] std::optional<Addressed> addressed(std::span<const std::uint8_t> bytes);

} // namespace vehicle_sil::control::wire
