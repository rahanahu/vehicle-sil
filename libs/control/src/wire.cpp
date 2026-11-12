// SPDX-License-Identifier: MIT
#include "vehicle_sil/control/wire.hpp"

#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::control::wire {
namespace {

// Every number is little endian, written byte by byte (no struct is copied).
//
// A message: 'V' 'S' 'C' 'W', version u16, kind u8, 0 u8, then the content of its kind. A text is
// its length u32 and its bytes. The beginning of a hello, up to its participant, never changes.
constexpr std::uint8_t kMagic[4] = {'V', 'S', 'C', 'W'};
constexpr std::size_t kHeaderBytes = 8;

constexpr std::uint8_t kStepFinal = 1;
constexpr std::uint8_t kStepValues = 2;

class Writer
{
public:
    explicit Writer(Bytes& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v)
    {
        u8(static_cast<std::uint8_t>(v));
        u8(static_cast<std::uint8_t>(v >> 8));
    }
    void u32(std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void u64(std::uint64_t v)
    {
        for (int i = 0; i < 8; ++i)
        {
            u8(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void i64(std::int64_t v) { u64(static_cast<std::uint64_t>(v)); }
    void f64(double v)
    {
        std::uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u64(bits);
    }
    void text(std::string_view v)
    {
        u32(static_cast<std::uint32_t>(v.size()));
        out_.insert(out_.end(), v.begin(), v.end());
    }
    void header(Kind kind)
    {
        out_.insert(out_.end(), std::begin(kMagic), std::end(kMagic));
        u16(kVersion);
        u8(static_cast<std::uint8_t>(kind));
        u8(0);
    }
    void value(const Value& v)
    {
        u8(static_cast<std::uint8_t>(v.index()));
        switch (v.index())
        {
        case 0:
            u8(std::get<bool>(v) ? 1 : 0);
            break;
        case 1:
            i64(std::get<std::int64_t>(v));
            break;
        default:
            f64(std::get<double>(v));
            break;
        }
    }
    /// Overwrites the two bytes at `at`.
    void patch_u16(std::size_t at, std::uint16_t v)
    {
        out_[at] = static_cast<std::uint8_t>(v);
        out_[at + 1] = static_cast<std::uint8_t>(v >> 8);
    }
    void patch_u32(std::size_t at, std::uint32_t v)
    {
        for (int i = 0; i < 4; ++i)
        {
            out_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }

private:
    Bytes& out_;
};

/// Reads within the span; the first read beyond it (or the first fail()) sticks.
class Reader
{
public:
    explicit Reader(std::span<const std::uint8_t> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return error_.code == ErrorCode::Ok; }
    [[nodiscard]] std::size_t left() const noexcept { return bytes_.size() - at_; }
    [[nodiscard]] Error take_error() { return std::move(error_); }
    void set_version(std::uint16_t version) noexcept { error_.version = version; }

    void fail(ErrorCode code, std::string text)
    {
        if (ok())
        {
            error_.code = code;
            error_.text = std::move(text);
        }
    }
    std::uint8_t u8()
    {
        if (!need(1))
        {
            return 0;
        }
        return bytes_[at_++];
    }
    std::uint16_t u16()
    {
        const std::uint16_t low = u8();
        return static_cast<std::uint16_t>(low | static_cast<std::uint16_t>(u8() << 8));
    }
    std::uint32_t u32()
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
        {
            v |= static_cast<std::uint32_t>(u8()) << (8 * i);
        }
        return v;
    }
    std::uint64_t u64()
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i)
        {
            v |= static_cast<std::uint64_t>(u8()) << (8 * i);
        }
        return v;
    }
    std::int64_t i64() { return static_cast<std::int64_t>(u64()); }
    double f64()
    {
        const std::uint64_t bits = u64();
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    std::string text()
    {
        std::string out;
        text(out);
        return out;
    }
    /// A text into `out` (its storage is reused).
    void text(std::string& out)
    {
        out.clear();
        const std::uint32_t size = u32();
        if (!ok())
        {
            return;
        }
        if (size > left())
        {
            fail(ErrorCode::Truncated, "a text of " + std::to_string(size) + " bytes with " + std::to_string(left()) + " left");
            return;
        }
        out.assign(reinterpret_cast<const char*>(bytes_.data() + at_), size);
        at_ += size;
    }
    /// A name: a text of 1..kMaxNameBytes bytes.
    void name(std::string& out, const char* what)
    {
        text(out);
        if (ok() && (out.empty() || out.size() > kMaxNameBytes))
        {
            fail(ErrorCode::Malformed, std::string(what) + " of " + std::to_string(out.size()) + " bytes; a name has 1 to " +
                                           std::to_string(kMaxNameBytes));
        }
    }
    /// A count of things of at least `least` bytes each: one that the rest of the message cannot
    /// hold is refused before anything is allocated for it.
    std::uint32_t count(std::size_t least, const char* what)
    {
        const std::uint32_t n = u32();
        if (ok() && n > left() / least)
        {
            fail(ErrorCode::Truncated, std::to_string(n) + " " + what + " announced with " + std::to_string(left()) + " bytes left");
            return 0;
        }
        return ok() ? n : 0;
    }
    /// An enumerator below `end`.
    std::uint8_t enumerator(std::uint8_t end, const char* what)
    {
        const std::uint8_t v = u8();
        if (ok() && v >= end)
        {
            fail(ErrorCode::Malformed, std::string("unknown ") + what + " " + std::to_string(v));
            return 0;
        }
        return v;
    }
    Value value()
    {
        switch (enumerator(3, "value type"))
        {
        case 0:
            return Value{u8() != 0};
        case 1:
            return Value{i64()};
        default:
            return Value{f64()};
        }
    }
    /// The header of a message of `kind`; false after an error (also another version: then the
    /// reader stands behind the header).
    bool header(Kind kind, std::size_t limit)
    {
        if (bytes_.size() < kHeaderBytes)
        {
            fail(bytes_.size() >= 4 && std::memcmp(bytes_.data(), kMagic, 4) != 0 ? ErrorCode::NotAMessage : ErrorCode::Truncated,
                 "a message has at least " + std::to_string(kHeaderBytes) + " bytes");
            return false;
        }
        if (std::memcmp(bytes_.data(), kMagic, 4) != 0)
        {
            fail(ErrorCode::NotAMessage, "it does not begin with VSCW");
            return false;
        }
        at_ = 4;
        const std::uint16_t version = u16();
        const std::uint8_t found = u8();
        static_cast<void>(u8());
        error_.version = version;
        if (found != static_cast<std::uint8_t>(kind))
        {
            fail(ErrorCode::WrongKind, "a message of kind " + std::to_string(found) + ", not " +
                                           std::to_string(static_cast<int>(kind)));
            return false;
        }
        // Before the version: a message of another version is not read beyond the limit either.
        if (bytes_.size() > limit)
        {
            fail(ErrorCode::TooLarge, std::to_string(bytes_.size()) + " bytes; the limit is " + std::to_string(limit));
            return false;
        }
        if (version != kVersion)
        {
            fail(ErrorCode::VersionMismatch, "written in version " + std::to_string(version) + " of the format; this side speaks version " +
                                                 std::to_string(kVersion));
            return false;
        }
        return true;
    }
    void end()
    {
        if (ok() && left() != 0)
        {
            fail(ErrorCode::Malformed, std::to_string(left()) + " bytes after the end of the message");
        }
    }

private:
    bool need(std::size_t n)
    {
        if (!ok())
        {
            return false;
        }
        if (left() < n)
        {
            fail(ErrorCode::Truncated, "the message ends after " + std::to_string(bytes_.size()) + " bytes");
            return false;
        }
        return true;
    }

    std::span<const std::uint8_t> bytes_;
    std::size_t at_ = 0;
    Error error_;
};

/// `text` cut to kMaxTextBytes, not inside a UTF-8 character.
std::string_view cut(std::string_view text)
{
    if (text.size() <= kMaxTextBytes)
    {
        return text;
    }
    std::size_t n = kMaxTextBytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xC0) == 0x80)
    {
        --n;
    }
    return text.substr(0, n);
}

constexpr std::uint8_t kHasMin = 1;
constexpr std::uint8_t kHasMax = 2;
constexpr std::uint8_t kHasStep = 4;

void write_variable(Writer& w, const VariableInfo& v)
{
    w.text(v.name);
    w.u8(static_cast<std::uint8_t>(v.type));
    w.u8(static_cast<std::uint8_t>(v.causality));
    w.text(v.unit);
    w.u8(static_cast<std::uint8_t>((v.min ? kHasMin : 0) | (v.max ? kHasMax : 0) | (v.step ? kHasStep : 0)));
    if (v.min)
    {
        w.f64(*v.min);
    }
    if (v.max)
    {
        w.f64(*v.max);
    }
    if (v.step)
    {
        w.f64(*v.step);
    }
    w.text(v.description);
    w.u32(static_cast<std::uint32_t>(v.items.size()));
    for (const auto& item : v.items)
    {
        w.text(item.name);
        w.i64(item.value);
    }
}

VariableInfo read_variable(Reader& r)
{
    VariableInfo v;
    r.name(v.name, "a variable name");
    v.type = static_cast<ValueType>(r.enumerator(4, "variable type"));
    v.causality = static_cast<Causality>(r.enumerator(3, "causality"));
    v.unit = r.text();
    const std::uint8_t has = r.u8();
    if (r.ok() && (has & ~(kHasMin | kHasMax | kHasStep)) != 0)
    {
        r.fail(ErrorCode::Malformed, "unknown flags of a variable");
    }
    if (has & kHasMin)
    {
        v.min = r.f64();
    }
    if (has & kHasMax)
    {
        v.max = r.f64();
    }
    if (has & kHasStep)
    {
        v.step = r.f64();
    }
    v.description = r.text();
    const std::uint32_t items = r.count(12, "enum items");
    v.items.reserve(items);
    for (std::uint32_t i = 0; i < items && r.ok(); ++i)
    {
        EnumItem item;
        item.name = r.text();
        item.value = r.i64();
        v.items.push_back(std::move(item));
    }
    return v;
}

// A result: seq u64, status u8, flags u8 (1 late, 2 applied_at), applied_at i64 if flagged, reason.
constexpr std::uint8_t kResultLate = 1;
constexpr std::uint8_t kResultAppliedAt = 2;

std::size_t result_bytes(const CommandResult& result)
{
    return 8 + 1 + 1 + (result.applied_at ? 8 : 0) + 4 + cut(result.reason).size();
}

void write_result(Writer& w, const CommandResult& result)
{
    w.u64(result.seq);
    w.u8(static_cast<std::uint8_t>(result.status));
    w.u8(static_cast<std::uint8_t>((result.late ? kResultLate : 0) | (result.applied_at ? kResultAppliedAt : 0)));
    if (result.applied_at)
    {
        w.i64(to_ns(*result.applied_at));
    }
    w.text(cut(result.reason));
}

std::size_t event_bytes(const Event& event)
{
    return 8 + 1 + 4 + cut(event.text).size();
}

void write_event(Writer& w, const Event& event)
{
    w.i64(to_ns(event.time));
    w.u8(static_cast<std::uint8_t>(event.level));
    w.text(cut(event.text));
}

std::size_t value_bytes(const Value& value)
{
    return value.index() == 0 ? 2 : 9;
}

SimTime time_of(std::int64_t ns)
{
    return SimTime{SimDuration{ns}};
}

} // namespace

std::optional<Header> peek(std::span<const std::uint8_t> bytes, Error& error)
{
    error = {};
    if (bytes.size() < kHeaderBytes)
    {
        error.code = bytes.size() >= 4 && std::memcmp(bytes.data(), kMagic, 4) != 0 ? ErrorCode::NotAMessage : ErrorCode::Truncated;
        error.text = "a message has at least " + std::to_string(kHeaderBytes) + " bytes";
        return std::nullopt;
    }
    if (std::memcmp(bytes.data(), kMagic, 4) != 0)
    {
        error.code = ErrorCode::NotAMessage;
        error.text = "it does not begin with VSCW";
        return std::nullopt;
    }
    Header header;
    header.version = static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8));
    error.version = header.version;
    if (bytes[6] < static_cast<std::uint8_t>(Kind::Hello) || bytes[6] > static_cast<std::uint8_t>(Kind::Ready))
    {
        // Another version may have kinds this one does not know.
        if (header.version == kVersion)
        {
            error.code = ErrorCode::WrongKind;
            error.text = "unknown kind " + std::to_string(bytes[6]);
            return std::nullopt;
        }
    }
    header.kind = static_cast<Kind>(bytes[6]);
    return header;
}

bool encode_hello(const Hello& hello, Bytes& out, std::string& why)
{
    out.clear();
    why.clear();
    if (hello.participant.empty() || hello.participant.size() > kMaxNameBytes)
    {
        why = "the participant name has " + std::to_string(hello.participant.size()) + " bytes; it must have 1 to " +
              std::to_string(kMaxNameBytes);
        return false;
    }
    if (hello.schema.size() > kMaxVariables)
    {
        why = "the ECU has " + std::to_string(hello.schema.size()) + " variables; at most " + std::to_string(kMaxVariables) +
              " can be offered";
        return false;
    }
    for (const auto& variable : hello.schema)
    {
        if (variable.name.empty() || variable.name.size() > kMaxNameBytes)
        {
            why = "the name of a variable has " + std::to_string(variable.name.size()) + " bytes (" +
                  variable.name.substr(0, 40) + "); it must have 1 to " + std::to_string(kMaxNameBytes);
            return false;
        }
    }
    if (hello.max_pending == 0 || hello.max_pending > kMaxPending)
    {
        why = "max_pending is " + std::to_string(hello.max_pending) + "; it must be within 1.." + std::to_string(kMaxPending);
        return false;
    }
    Writer w(out);
    w.header(Kind::Hello);
    w.text(hello.participant); // up to here, the same in every version
    w.u64(hello.session);
    w.u8(hello.virtual_time ? 1 : 0);
    w.i64(hello.step.count());
    w.i64(hello.sample_period.count());
    w.u32(static_cast<std::uint32_t>(hello.max_pending));
    w.u32(static_cast<std::uint32_t>(hello.schema.size()));
    for (const auto& variable : hello.schema)
    {
        write_variable(w, variable);
        if (out.size() > kMaxHelloBytes)
        {
            break;
        }
    }
    if (out.size() > kMaxHelloBytes)
    {
        out.clear();
        why = "the description of the " + std::to_string(hello.schema.size()) + " variables is larger than " +
              std::to_string(kMaxHelloBytes) + " bytes: shorten the descriptions or declare fewer variables";
        return false;
    }
    return true;
}

Error decode_hello(std::span<const std::uint8_t> bytes, Hello& out)
{
    out.participant.clear();
    out.schema.clear();
    Reader r(bytes);
    if (!r.header(Kind::Hello, kMaxHelloBytes))
    {
        Error error = r.take_error();
        if (error.code == ErrorCode::VersionMismatch)
        {
            // The participant is at the same place in every version.
            Reader name(bytes.subspan(kHeaderBytes));
            name.name(out.participant, "the participant name");
            if (!name.ok())
            {
                out.participant.clear();
            }
        }
        return error;
    }
    r.name(out.participant, "the participant name");
    out.session = r.u64();
    out.virtual_time = r.u8() != 0;
    out.step = SimDuration{r.i64()};
    out.sample_period = SimDuration{r.i64()};
    out.max_pending = r.u32();
    // A variable has at least a name of one byte.
    const std::uint32_t variables = r.count(4 + 1 + 1 + 1 + 4 + 1 + 4 + 4, "variables");
    if (r.ok() && variables > kMaxVariables)
    {
        r.fail(ErrorCode::TooLarge, std::to_string(variables) + " variables; the limit is " + std::to_string(kMaxVariables));
    }
    // Nothing is reserved for the number announced: the storage grows with what is really there.
    for (std::uint32_t i = 0; i < variables && r.ok(); ++i)
    {
        out.schema.push_back(read_variable(r));
    }
    r.end();
    if (r.ok() && (out.max_pending == 0 || out.max_pending > kMaxPending))
    {
        r.fail(ErrorCode::Malformed, "max_pending is " + std::to_string(out.max_pending));
    }
    if (r.ok() && (out.step < SimDuration::zero() || out.sample_period < SimDuration::zero()))
    {
        r.fail(ErrorCode::Malformed, "a negative step or sample period");
    }
    return r.take_error();
}

bool encode_command(const CommandMessage& message, Bytes& out)
{
    out.clear();
    Writer w(out);
    w.header(Kind::Command);
    w.u64(message.session);
    w.u64(message.command.seq);
    w.u8(message.command.at ? 1 : 0);
    w.i64(message.command.at ? to_ns(*message.command.at) : 0);
    w.u8(static_cast<std::uint8_t>(message.command.late));
    w.u32(static_cast<std::uint32_t>(message.command.writes.size()));
    for (const auto& write : message.command.writes)
    {
        w.text(write.name);
        w.value(write.value);
        if (out.size() > kMaxMessageBytes)
        {
            out.clear();
            return false;
        }
    }
    return true;
}

Error decode_command(std::span<const std::uint8_t> bytes, CommandMessage& out)
{
    out.command.at.reset();
    out.command.writes.clear();
    Reader r(bytes);
    if (!r.header(Kind::Command, kMaxMessageBytes))
    {
        return r.take_error();
    }
    out.session = r.u64();
    out.command.seq = r.u64();
    const bool has_at = r.u8() != 0;
    const std::int64_t at = r.i64();
    if (has_at)
    {
        out.command.at = time_of(at);
    }
    out.command.late = static_cast<LatePolicy>(r.enumerator(2, "late policy"));
    const std::uint32_t writes = r.count(4 + 2, "writes");
    for (std::uint32_t i = 0; i < writes && r.ok(); ++i)
    {
        Write& write = out.command.writes.emplace_back();
        r.text(write.name);
        write.value = r.value();
    }
    r.end();
    return r.take_error();
}

void encode_submitted(const SubmittedMessage& message, Bytes& out)
{
    out.clear();
    Writer w(out);
    w.header(Kind::Submitted);
    w.u64(message.session);
    w.u64(message.seq);
    w.u8(static_cast<std::uint8_t>(message.result.status));
    w.text(cut(message.result.reason));
}

Error decode_submitted(std::span<const std::uint8_t> bytes, SubmittedMessage& out)
{
    Reader r(bytes);
    if (!r.header(Kind::Submitted, kMaxMessageBytes))
    {
        return r.take_error();
    }
    out.session = r.u64();
    out.seq = r.u64();
    out.result.status = static_cast<SubmitStatus>(r.enumerator(3, "submit status"));
    r.text(out.result.reason);
    r.end();
    return r.take_error();
}

void encode_session(Kind kind, std::uint64_t session, Bytes& out)
{
    if (kind != Kind::Probe && kind != Kind::Ready)
    {
        throw std::invalid_argument("vehicle_sil::control::wire::encode_session(): the kind must be Probe or Ready");
    }
    out.clear();
    Writer w(out);
    w.header(kind);
    w.u64(session);
}

std::optional<Addressed> addressed(std::span<const std::uint8_t> bytes)
{
    if (bytes.size() < kHeaderBytes + 8 || bytes.size() > kMaxMessageBytes || std::memcmp(bytes.data(), kMagic, 4) != 0 ||
        static_cast<std::uint16_t>(bytes[4] | (bytes[5] << 8)) != kVersion)
    {
        return std::nullopt;
    }
    Addressed out;
    switch (bytes[6])
    {
    case static_cast<std::uint8_t>(Kind::Command):
        out.kind = Kind::Command;
        break;
    case static_cast<std::uint8_t>(Kind::Probe):
        out.kind = Kind::Probe;
        break;
    case static_cast<std::uint8_t>(Kind::Ready):
        out.kind = Kind::Ready;
        break;
    default:
        return std::nullopt;
    }
    if (out.kind != Kind::Command && bytes.size() != kHeaderBytes + 8)
    {
        return std::nullopt; // a probe and a ready message are their session and nothing else
    }
    for (int i = 0; i < 8; ++i)
    {
        out.session |= static_cast<std::uint64_t>(bytes[kHeaderBytes + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return out;
}

// A step message: session u64, time i64, flags u8, results u16, events u16, the results, the
// events, and with kStepValues: time i64, sequence u64, total u32, first u32, count u32, values.
void encode_step(std::uint64_t session, SimTime time, bool final, std::span<const CommandResult> results,
                 std::span<const Event> events, const Snapshot* sample, std::size_t max_bytes, Bytes& scratch,
                 const std::function<void(const Bytes& message)>& emit)
{
    if (max_bytes < kMinMessageBytes || max_bytes > kMaxMessageBytes)
    {
        throw std::invalid_argument("vehicle_sil::control::wire::encode_step(): max_bytes must be within " +
                                    std::to_string(kMinMessageBytes) + ".." + std::to_string(kMaxMessageBytes));
    }
    constexpr std::size_t kFlagsAt = kHeaderBytes + 16;
    constexpr std::size_t kValuesHead = 8 + 8 + 4 + 4 + 4;
    Writer w(scratch);
    std::size_t next_result = 0;
    std::size_t next_event = 0;
    std::size_t next_value = 0;
    const std::size_t values = sample != nullptr ? sample->values.size() : 0;
    bool sample_left = sample != nullptr;
    while (true)
    {
        scratch.clear();
        w.header(Kind::Step);
        w.u64(session);
        w.i64(to_ns(time));
        w.u8(0);
        w.u16(0);
        w.u16(0);
        std::uint16_t n_results = 0;
        while (next_result < results.size() && n_results < std::numeric_limits<std::uint16_t>::max()
               && scratch.size() + result_bytes(results[next_result]) <= max_bytes)
        {
            write_result(w, results[next_result++]);
            ++n_results;
        }
        std::uint16_t n_events = 0;
        if (next_result == results.size())
        {
            while (next_event < events.size() && n_events < std::numeric_limits<std::uint16_t>::max()
                   && scratch.size() + event_bytes(events[next_event]) <= max_bytes)
            {
                write_event(w, events[next_event++]);
                ++n_events;
            }
        }
        std::uint8_t flags = 0;
        // The values begin only when the results and events are out, and a part needs room for
        // its head and one value (or is the whole of an empty sample).
        if (sample_left && next_result == results.size() && next_event == events.size()
            && scratch.size() + kValuesHead + (values == 0 ? 0 : 9) <= max_bytes)
        {
            flags |= kStepValues;
            w.i64(to_ns(sample->time));
            w.u64(sample->sequence);
            w.u32(static_cast<std::uint32_t>(values));
            w.u32(static_cast<std::uint32_t>(next_value));
            const std::size_t count_at = scratch.size();
            w.u32(0);
            std::uint32_t n = 0;
            while (next_value < values && scratch.size() + value_bytes(sample->values[next_value]) <= max_bytes)
            {
                w.value(sample->values[next_value++]);
                ++n;
            }
            w.patch_u32(count_at, n);
            sample_left = next_value < values;
        }
        const bool last = next_result == results.size() && next_event == events.size() && !sample_left;
        if (last && final)
        {
            flags |= kStepFinal;
        }
        scratch[kFlagsAt] = flags;
        w.patch_u16(kFlagsAt + 1, n_results);
        w.patch_u16(kFlagsAt + 3, n_events);
        if (n_results > 0 || n_events > 0 || (flags & (kStepValues | kStepFinal)) != 0)
        {
            emit(scratch);
        }
        if (last)
        {
            return;
        }
    }
}

Error decode_step(std::span<const std::uint8_t> bytes, StepMessage& out)
{
    out.results.clear();
    out.events.clear();
    out.has_values = false;
    out.values.values.clear();
    Reader r(bytes);
    if (!r.header(Kind::Step, kMaxMessageBytes))
    {
        return r.take_error();
    }
    out.session = r.u64();
    out.time = time_of(r.i64());
    const std::uint8_t flags = r.u8();
    if (r.ok() && (flags & ~(kStepFinal | kStepValues)) != 0)
    {
        r.fail(ErrorCode::Malformed, "unknown flags of a step message");
    }
    out.final = (flags & kStepFinal) != 0;
    const std::uint16_t results = r.u16();
    const std::uint16_t events = r.u16();
    if (r.ok() && (results > r.left() / 14 || events > r.left() / 13))
    {
        r.fail(ErrorCode::Truncated, "more results or events announced than the message can hold");
    }
    if (!r.ok())
    {
        return r.take_error();
    }
    for (std::uint16_t i = 0; i < results && r.ok(); ++i)
    {
        CommandResult& result = out.results.emplace_back();
        result.seq = r.u64();
        result.status = static_cast<CommandStatus>(r.enumerator(4, "command status"));
        const std::uint8_t has = r.u8();
        if (r.ok() && (has & ~(kResultLate | kResultAppliedAt)) != 0)
        {
            r.fail(ErrorCode::Malformed, "unknown flags of a result");
        }
        result.late = (has & kResultLate) != 0;
        if (has & kResultAppliedAt)
        {
            result.applied_at = time_of(r.i64());
        }
        r.text(result.reason);
    }
    for (std::uint16_t i = 0; i < events && r.ok(); ++i)
    {
        Event& event = out.events.emplace_back();
        event.time = time_of(r.i64());
        event.level = static_cast<EventLevel>(r.enumerator(3, "event level"));
        r.text(event.text);
    }
    if (r.ok() && (flags & kStepValues) != 0)
    {
        ValuesPart& part = out.values;
        part.time = time_of(r.i64());
        part.sequence = r.u64();
        part.total = r.u32();
        part.first = r.u32();
        const std::uint32_t count = r.count(2, "values");
        if (r.ok() && (part.first > part.total || count > part.total - part.first))
        {
            r.fail(ErrorCode::Malformed, "a part of a sample beyond its end");
        }
        for (std::uint32_t i = 0; i < count && r.ok(); ++i)
        {
            part.values.push_back(r.value());
        }
        out.has_values = true;
    }
    r.end();
    return r.take_error();
}

} // namespace vehicle_sil::control::wire
