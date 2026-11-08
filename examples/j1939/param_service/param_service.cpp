// SPDX-License-Identifier: MIT
#include "param_service.hpp"

#include "param_service_messages.hpp"
#include "vehicle_sil/j1939/codec_node.hpp"

#include <algorithm>
#include <utility>

namespace param_service {

namespace msg = messages;
using j1939::ExchangeOutcome;
using j1939::ExchangeResult;
using j1939::Message;
using j1939::ReplyMatch;

namespace {

// Operation bytes (the selectors of the YAML, and the two block messages written as bytes).
constexpr std::uint8_t kOpen = 0x10;
constexpr std::uint8_t kRead = 0x11;
constexpr std::uint8_t kWrite = 0x12;
constexpr std::uint8_t kReadBlock = 0x13;
constexpr std::uint8_t kClose = 0x15;
constexpr std::uint8_t kOpenOk = 0x90;
constexpr std::uint8_t kReadOk = 0x91;
constexpr std::uint8_t kWriteOk = 0x92;
constexpr std::uint8_t kBlockWritten = 0x94;
constexpr std::uint8_t kClosed = 0x95;

using Code = msg::Error::Code;

/// Reads, Open and Close may run twice: sent again after a timeout (ParamClientOptions::retries)
/// and from the tool's new address if it moves.
const j1939::CommandOptions kRepeatable{.on_address_change = j1939::AddressChangePolicy::SendAgain};
/// Writes are sent once: no retries, and Interrupted (the default AddressChangePolicy::Stop) if
/// the tool moves, as the ECU may have written already.
const j1939::CommandOptions kOnce{.retries = 0};

/// What an answer names after its operation and session bytes (byte 2 on): the parameter (2
/// bytes, little endian) or the block (1 byte) of the command, so that a late answer to an
/// earlier command (sent again after a timeout) is not taken for this one's.
struct Item
{
    std::uint16_t value = 0;
    std::size_t bytes = 0; ///< 0: nothing to compare.
};

/// The answer to the command with operation byte `op`: the expected answer (operation byte `ok`,
/// for `session` if given, naming `item`), or an Error or a Pending for that command.
j1939::ReplyMatcher answer_to(std::uint8_t op, std::uint8_t ok, std::optional<std::uint8_t> session, Item item = {})
{
    return [op, ok, session, item](const Message& m) {
        if (m.pgn != kPgn || m.data.empty())
        {
            return ReplyMatch::Ignore;
        }
        if (const auto e = msg::Error::decode(m.data); e && e->request.raw() == op)
        {
            return ReplyMatch::Final;
        }
        if (const auto p = msg::Pending::decode(m.data); p && p->request.raw() == op)
        {
            return ReplyMatch::Pending;
        }
        const bool sameSession = !session || (m.data.size() > 1 && m.data[1] == *session);
        const bool sameItem = item.bytes == 0 ||
                              (m.data.size() >= 2 + item.bytes && m.data[2] == static_cast<std::uint8_t>(item.value) &&
                               (item.bytes == 1 || m.data[3] == static_cast<std::uint8_t>(item.value >> 8)));
        return m.data[0] == ok && sameSession && sameItem ? ReplyMatch::Final : ReplyMatch::Ignore;
    };
}

ParamStatus status_of(Code code)
{
    switch (code)
    {
    case Code::busy:
        return ParamStatus::Busy;
    case Code::unknown_parameter:
        return ParamStatus::UnknownParameter;
    case Code::read_only:
        return ParamStatus::ReadOnly;
    case Code::out_of_range:
        return ParamStatus::OutOfRange;
    case Code::denied:
        return ParamStatus::Denied;
    default:
        return ParamStatus::Failed;
    }
}

ParamStatus status_of(const ExchangeResult& r)
{
    switch (r.outcome)
    {
    case ExchangeOutcome::Timeout:
        return ParamStatus::Timeout;
    case ExchangeOutcome::PeerLost:
        return ParamStatus::ServerLost;
    case ExchangeOutcome::NotSent:
        return ParamStatus::NotSent;
    case ExchangeOutcome::Interrupted:
        return ParamStatus::Interrupted;
    case ExchangeOutcome::Reply:
        break;
    }
    if (const auto e = msg::Error::decode(r.reply.data); e && e->code.value())
    {
        return status_of(*e->code.value());
    }
    return ParamStatus::Failed;
}

} // namespace

// --- ParamServer -------------------------------------------------------------------------

ParamServer::ParamServer(j1939::Node& node, ParameterTable& table, ParamServerOptions options)
  : node_(node),
    table_(table),
    options_(options),
    commands_(node, node.on_message(kPgn, [this](const Message& m) { on_command(m); })),
    claims_(node, node.on_message(j1939::kPgnAddressClaimed, [this](const Message&) { on_claim(); })),
    address_(node, node.on_address_change([this](const j1939::AddressChange& c) {
                 if (open_ && (c.address != c.previous_address || !node_.can_send_now()))
                 {
                     ++statistics_.lost; // the tool's commands went to the old address
                     end_session();
                 }
             })),
    idle_(node, j1939::TimerKind::Timeout,
          [this] {
              ++statistics_.expired;
              end_session();
          }),
    pending_(node, j1939::TimerKind::Scheduled,
             [this] {
                 msg::Pending p;
                 p.request.set_raw(kWrite);
                 p.session.set_raw(session_);
                 j1939::codec::send(node_, p, client_);
                 pending_.start_after(options_.pending_interval);
             }),
    written_(node, j1939::TimerKind::Scheduled, [this] { finish_write(); })
{
    out_.reserve(3 + table_.max_block);
}

void ParamServer::on_command(const Message& m)
{
    if (m.destination != node_.address() || m.data.empty())
    {
        return;
    }
    const std::uint8_t op = m.data[0];
    if (op == kOpen)
    {
        open(m);
        return;
    }
    if (op != kRead && op != kWrite && op != kReadBlock && op != kWriteBlock && op != kClose)
    {
        return; // an answer or another use of the group
    }
    if (!open_ || m.source != client_ || m.data.size() < 2 || m.data[1] != session_)
    {
        error(op, static_cast<std::uint8_t>(Code::no_session), m.source); // maybe another tool
        return;
    }
    idle_.start_after(options_.idle_timeout); // from this command
    switch (op)
    {
    case kRead: {
        const auto read = msg::Read::decode(m.data);
        const auto found = read ? table_.parameters.find(static_cast<std::uint16_t>(read->parameter.raw()))
                                : table_.parameters.end();
        if (found == table_.parameters.end())
        {
            error(op, static_cast<std::uint8_t>(read ? Code::unknown_parameter : Code::bad_request));
            return;
        }
        msg::ReadOk ok;
        ok.session.set_raw(session_);
        ok.parameter.set_raw(found->first);
        ok.value.set_raw(found->second.value);
        j1939::codec::send(node_, ok, client_);
        return;
    }
    case kWrite:
        write(m);
        return;
    case kReadBlock: {
        const auto read = msg::ReadBlock::decode(m.data);
        const auto found = read ? table_.blocks.find(static_cast<std::uint8_t>(read->block.raw())) : table_.blocks.end();
        if (found == table_.blocks.end())
        {
            error(op, static_cast<std::uint8_t>(read ? Code::unknown_parameter : Code::bad_request));
            return;
        }
        out_.assign({kBlockData, session_, found->first});
        out_.insert(out_.end(), found->second.begin(), found->second.end());
        // Longer than 8 bytes: the transport protocol; wait behind another transfer to the tool.
        static_cast<void>(node_.send(kPgn, out_, client_, j1939::kDefaultPriority, j1939::WhenBusy::Queue));
        return;
    }
    case kWriteBlock:
        write_block(m);
        return;
    default: { // kClose
        msg::Closed closed;
        closed.session.set_raw(session_);
        j1939::codec::send(node_, closed, client_);
        end_session();
        return;
    }
    }
}

void ParamServer::open(const Message& m)
{
    const auto request = msg::Open::decode(m.data);
    if (!request || !request->access.value())
    {
        error(kOpen, static_cast<std::uint8_t>(Code::bad_request), m.source);
        return;
    }
    if (open_ && m.source != client_)
    {
        error(kOpen, static_cast<std::uint8_t>(Code::busy), m.source);
        return;
    }
    if (!open_) // otherwise the same tool asks again: the same session
    {
        open_ = true;
        client_ = m.source;
        client_name_ = node_.network().name_at(m.source); // to see it leave its address
        session_ = static_cast<std::uint8_t>(session_ == 0xFF ? 1 : session_ + 1);
        ++statistics_.sessions;
    }
    write_access_ = *request->access.value() == msg::Open::Access::write;
    idle_.start_after(options_.idle_timeout);
    msg::OpenOk ok;
    ok.session.set_raw(session_);
    ok.idle_timeout.set_raw(static_cast<std::uint64_t>(options_.idle_timeout.count()));
    j1939::codec::send(node_, ok, client_);
}

void ParamServer::write(const Message& m)
{
    const auto request = msg::Write::decode(m.data);
    if (!request)
    {
        error(kWrite, static_cast<std::uint8_t>(Code::bad_request));
        return;
    }
    const auto id = static_cast<std::uint16_t>(request->parameter.raw());
    const auto value = static_cast<std::uint32_t>(request->value.raw());
    const auto found = table_.parameters.find(id);
    const Code refusal = !write_access_                            ? Code::denied
                         : found == table_.parameters.end()        ? Code::unknown_parameter
                         : !found->second.writable                 ? Code::read_only
                         : value < found->second.min || value > found->second.max ? Code::out_of_range
                                                                   : Code{};
    if (refusal != Code{})
    {
        error(kWrite, static_cast<std::uint8_t>(refusal));
        return;
    }
    if (writing_ && (id != write_parameter_ || value != write_value_))
    {
        error(kWrite, static_cast<std::uint8_t>(Code::busy));
        return;
    }
    if (!writing_)
    {
        writing_ = true;
        write_parameter_ = id;
        write_value_ = value;
        written_.start_after(options_.write_time);
    }
    // Pending now (also to a repeated Write) and every pending_interval until the write is done.
    pending_.start_after(vehicle_sil::SimDuration::zero());
}

void ParamServer::finish_write()
{
    writing_ = false;
    pending_.cancel();
    table_.parameters[write_parameter_].value = write_value_;
    if (open_)
    {
        msg::WriteOk ok;
        ok.session.set_raw(session_);
        ok.parameter.set_raw(write_parameter_);
        j1939::codec::send(node_, ok, client_);
    }
}

void ParamServer::write_block(const Message& m)
{
    if (!write_access_)
    {
        error(kWriteBlock, static_cast<std::uint8_t>(Code::denied));
        return;
    }
    if (m.data.size() < 3)
    {
        error(kWriteBlock, static_cast<std::uint8_t>(Code::bad_request));
        return;
    }
    const std::uint8_t block = m.data[2];
    const auto data = m.data.subspan(3);
    if (data.size() > table_.max_block)
    {
        error(kWriteBlock, static_cast<std::uint8_t>(Code::out_of_range));
        return;
    }
    table_.blocks[block].assign(data.begin(), data.end());
    msg::BlockWritten ok;
    ok.session.set_raw(session_);
    ok.block.set_raw(block);
    ok.length.set_raw(data.size());
    j1939::codec::send(node_, ok, client_);
}

void ParamServer::on_claim()
{
    // The address table already shows the claim: has the tool's NAME left its address?
    if (open_ && client_name_ && node_.network().address_of(*client_name_) != client_)
    {
        ++statistics_.lost;
        end_session();
    }
}

void ParamServer::end_session()
{
    open_ = false;
    client_name_.reset();
    idle_.cancel();
    if (writing_) // the tool is gone; the write is dropped
    {
        writing_ = false;
        pending_.cancel();
        written_.cancel();
    }
}

void ParamServer::error(std::uint8_t request, std::uint8_t code, std::optional<j1939::Address> to)
{
    msg::Error e;
    e.request.set_raw(request);
    e.session.set_raw(open_ ? session_ : 0);
    e.code.set_raw(code);
    ++statistics_.errors;
    j1939::codec::send(node_, e, to.value_or(client_));
}

// --- ParamClient -------------------------------------------------------------------------

ParamClient::ParamClient(j1939::Node& node, j1939::Name server, ParamClientOptions options)
  : server_(server),
    options_(options),
    exchange_(node, j1939::ExchangeOptions{.timeout = options.timeout,
                                           .retries = options.retries,
                                           .capacity = 1, // one command at a time
                                           .max_length = std::max<std::size_t>(8, 3 + options.max_block)})
{
}

bool ParamClient::read(std::uint16_t parameter, ValueDone done)
{
    return enqueue(Operation{.kind = Kind::Read, .id = parameter, .value_done = std::move(done)});
}

bool ParamClient::write(std::uint16_t parameter, std::uint32_t value, Done done)
{
    return enqueue(Operation{.kind = Kind::Write, .id = parameter, .value = value, .done = std::move(done)});
}

bool ParamClient::read_block(std::uint8_t block, BlockDone done)
{
    return enqueue(Operation{.kind = Kind::ReadBlock, .id = block, .block_done = std::move(done)});
}

bool ParamClient::write_block(std::uint8_t block, std::span<const std::uint8_t> data, Done done)
{
    if (data.size() > options_.max_block)
    {
        return false;
    }
    return enqueue(Operation{.kind = Kind::WriteBlock, .id = block, .data = {data.begin(), data.end()}, .done = std::move(done)});
}

bool ParamClient::close(Done done)
{
    return enqueue(Operation{.kind = Kind::Close, .done = std::move(done)});
}

bool ParamClient::enqueue(Operation operation)
{
    if (queue_.size() >= options_.queue_capacity)
    {
        return false;
    }
    queue_.push_back(std::move(operation));
    next();
    return true;
}

void ParamClient::next()
{
    if (running_ || queue_.empty())
    {
        return;
    }
    running_ = true;
    // A Close without a session is sent anyway (the ECU answers "no session"): the callbacks
    // keep their order, and none is called from inside read(), write(), ... .
    if (session_ || queue_.front().kind == Kind::Close)
    {
        run();
    }
    else
    {
        open();
    }
}

void ParamClient::open()
{
    msg::Open command;
    command.access.set(options_.write_access ? msg::Open::Access::write : msg::Open::Access::read);
    const auto opened = [this](const ExchangeResult& r) {
        const auto ok = r.outcome == ExchangeOutcome::Reply ? msg::OpenOk::decode(r.reply.data) : std::nullopt;
        if (!ok)
        {
            complete(status_of(r));
            return;
        }
        session_ = static_cast<std::uint8_t>(ok->session.raw());
        run();
    };
    exchange_.start(kPgn, command.bytes(), server_, answer_to(kOpen, kOpenOk, std::nullopt), opened, kRepeatable);
}

void ParamClient::run()
{
    const Operation& op = queue_.front();
    const std::uint8_t session = session_.value_or(0);
    const auto done = [this](const ExchangeResult& r) { on_answer(r); };
    switch (op.kind)
    {
    case Kind::Read: {
        msg::Read c;
        c.session.set_raw(session);
        c.parameter.set_raw(op.id);
        exchange_.start(kPgn, c.bytes(), server_, answer_to(kRead, kReadOk, session, Item{op.id, 2}), done, kRepeatable);
        return;
    }
    case Kind::Write: {
        msg::Write c;
        c.session.set_raw(session);
        c.parameter.set_raw(op.id);
        c.value.set_raw(op.value);
        exchange_.start(kPgn, c.bytes(), server_, answer_to(kWrite, kWriteOk, session, Item{op.id, 2}), done, kOnce);
        return;
    }
    case Kind::ReadBlock: {
        msg::ReadBlock c;
        c.session.set_raw(session);
        c.block.set_raw(op.id);
        exchange_.start(kPgn, c.bytes(), server_, answer_to(kReadBlock, kBlockData, session, Item{op.id, 1}), done,
                        j1939::CommandOptions{.long_answer = kPgn, // the block comes with the transport protocol
                                              .on_address_change = j1939::AddressChangePolicy::SendAgain});
        return;
    }
    case Kind::WriteBlock: {
        std::vector<std::uint8_t> c{kWriteBlock, session, static_cast<std::uint8_t>(op.id)};
        c.insert(c.end(), op.data.begin(), op.data.end());
        exchange_.start(kPgn, c, server_, answer_to(kWriteBlock, kBlockWritten, session, Item{op.id, 1}), done, kOnce);
        return;
    }
    case Kind::Close: {
        msg::Close c;
        c.session.set_raw(session);
        exchange_.start(kPgn, c.bytes(), server_, answer_to(kClose, kClosed, session), done, kRepeatable);
        return;
    }
    }
}

void ParamClient::on_answer(const ExchangeResult& r)
{
    Operation& op = queue_.front();
    if (r.outcome != ExchangeOutcome::Reply)
    {
        session_.reset(); // the ECU's state is unknown: open again for the next operation
        complete(status_of(r));
        return;
    }
    if (const auto e = msg::Error::decode(r.reply.data))
    {
        if (e->code.value() == Code::no_session)
        {
            session_.reset();
            if (op.kind == Kind::Close)
            {
                complete(ParamStatus::Ok); // nothing to close
                return;
            }
            if (!op.reopened) // the session ended (idle_timeout): open a new one, once
            {
                op.reopened = true;
                open();
                return;
            }
        }
        complete(status_of(r));
        return;
    }
    switch (op.kind)
    {
    case Kind::Read: {
        const auto ok = msg::ReadOk::decode(r.reply.data);
        complete(ParamStatus::Ok, ok ? static_cast<std::uint32_t>(ok->value.raw()) : 0);
        return;
    }
    case Kind::ReadBlock:
        complete(ParamStatus::Ok, 0, r.reply.data.subspan(std::min<std::size_t>(3, r.reply.data.size())));
        return;
    case Kind::Close:
        session_.reset();
        complete(ParamStatus::Ok);
        return;
    default:
        complete(ParamStatus::Ok);
        return;
    }
}

void ParamClient::complete(ParamStatus status, std::uint32_t value, std::span<const std::uint8_t> data)
{
    // State first: the next operation starts before the callback, which may queue more or
    // destroy the client.
    Operation op = std::move(queue_.front());
    queue_.pop_front();
    running_ = false;
    next();
    if (op.value_done)
    {
        op.value_done(status, value);
    }
    else if (op.block_done)
    {
        op.block_done(status, data);
    }
    else if (op.done)
    {
        op.done(status);
    }
}

} // namespace param_service
