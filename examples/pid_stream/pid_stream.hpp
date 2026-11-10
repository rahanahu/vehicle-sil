// SPDX-License-Identifier: MIT
//
// A small protocol of this example's own, written as two parts that use the public API of
// vehicle_sil::j1939 and nothing else: a client asks an ECU for the values of some of its
// parameters, and the ECU then sends them to the client every period until it is told to stop.
//
//   client -> ECU   Start {period in ms, ids...} | Stop            (one request per client)
//   ECU -> client   Data {sequence, (id, value)...} every period    (longer than 8 bytes: transport protocol)
//
// The group is Proprietary A with data page 1 (PGN 0x1EF00). The ids and the byte layout are made
// up for this example. The layout is the `Format`: four small functions that can be replaced.
// A stream ends with Stop, when the client's NAME leaves its address, when the ECU's own address
// changes, or (if `lease` is set) when the client does not repeat its Start in time.
#pragma once

#include "vehicle_sil/j1939/periodic_sender.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace pid_stream {

namespace j1939 = vehicle_sil::j1939;

inline constexpr j1939::Pgn kPgn{0x1EF00u};

struct Reading
{
    std::uint8_t id = 0;
    std::uint16_t value = 0;
};

struct Request
{
    bool stop = false;
    std::chrono::milliseconds period{};
    std::vector<std::uint8_t> ids{};
};

struct Reply
{
    std::uint8_t sequence = 0;
    std::vector<Reading> readings{};
    vehicle_sil::SimTime time{}; ///< Set by the client: when the ECU sent it.
};

/// The byte layout of the protocol. The writers append to `out`; the readers return nothing for
/// bytes that are not theirs. Replace members to change the layout; the parts do not look at bytes.
struct Format
{
    std::function<void(std::vector<std::uint8_t>& out, const Request&)> write_request = [](auto& out, const Request& r) {
        if (r.stop)
        {
            out.push_back(0x02);
            return;
        }
        if (r.period.count() < 0 || r.period.count() > 0xFFFF) // the field has 16 bits: do not truncate silently
        {
            throw std::invalid_argument("pid_stream: the period must fit in 16 bits of milliseconds (0..65535)");
        }
        out.insert(out.end(), {0x01, static_cast<std::uint8_t>(r.period.count()), static_cast<std::uint8_t>(r.period.count() >> 8)});
        out.insert(out.end(), r.ids.begin(), r.ids.end());
    };
    std::function<std::optional<Request>(std::span<const std::uint8_t>)> read_request =
        [](std::span<const std::uint8_t> d) -> std::optional<Request> {
        if (d.size() == 1 && d[0] == 0x02)
        {
            return Request{.stop = true};
        }
        if (d.size() < 4 || d[0] != 0x01)
        {
            return std::nullopt;
        }
        return Request{false, std::chrono::milliseconds(d[1] | (d[2] << 8)), {d.begin() + 3, d.end()}};
    };
    std::function<void(std::vector<std::uint8_t>& out, std::uint8_t sequence, std::span<const Reading>)> write_reply =
        [](auto& out, std::uint8_t sequence, std::span<const Reading> readings) {
            out.insert(out.end(), {0x81, sequence});
            for (const Reading& r : readings)
            {
                out.insert(out.end(), {r.id, static_cast<std::uint8_t>(r.value), static_cast<std::uint8_t>(r.value >> 8)});
            }
        };
    std::function<std::optional<Reply>(std::span<const std::uint8_t>)> read_reply =
        [](std::span<const std::uint8_t> d) -> std::optional<Reply> {
        if (d.size() < 2 || d[0] != 0x81 || (d.size() - 2) % 3 != 0)
        {
            return std::nullopt;
        }
        Reply reply{.sequence = d[1]};
        for (std::size_t i = 2; i < d.size(); i += 3)
        {
            reply.readings.push_back({d[i], static_cast<std::uint16_t>(d[i + 1] | (d[i + 2] << 8))});
        }
        return reply;
    };
    std::size_t max_reply = 64; ///< The longest reply a stream may write (bytes).
};

/// The ECU's parameters: id -> the current value. An id that is not in the table reads 0xFFFF.
using PidTable = std::map<std::uint8_t, std::function<std::uint16_t()>>;

struct StreamerOptions
{
    Format format{};
    std::size_t max_ids = 16;                 ///< Ids per stream; longer requests are ignored.
    std::size_t max_streams = 8;              ///< Clients served at once (each stream uses timers of the network).
    std::chrono::milliseconds min_period{10}; ///< Shorter periods are ignored.
    std::chrono::milliseconds lease{0};       ///< > 0: a stream ends this long after the client's last Start.
};

/// The ECU's side: node.attach<PidStreamer>(table, options). One stream per client; a new Start
/// from the same client replaces its stream.
class PidStreamer
{
public:
    PidStreamer(j1939::Node& node, PidTable table, StreamerOptions options = {})
      : node_(node),
        table_(std::move(table)),
        options_(std::move(options)),
        commands_(node, node.on_message(kPgn, [this](const j1939::Message& m) { on_command(m); })),
        claims_(node, node.on_message(j1939::kPgnAddressClaimed, [this](const j1939::Message&) { drop_gone_clients(); })),
        address_(node, node.on_address_change([this](const j1939::AddressChange& c) {
            if (c.address != c.previous_address || !node_.can_send_now())
            {
                streams_.clear(); // the clients addressed the old address
            }
        }))
    {
    }

    [[nodiscard]] std::size_t streams() const noexcept { return streams_.size(); }

private:
    struct Stream
    {
        std::vector<std::uint8_t> ids;
        std::uint8_t sequence = 0;
        std::optional<j1939::Name> client_name;
        std::unique_ptr<j1939::PeriodicSender> sender;
        std::unique_ptr<j1939::Timer> lease;
    };

    void on_command(const j1939::Message& m)
    {
        if (m.destination != node_.address())
        {
            return;
        }
        const auto request = options_.format.read_request(m.data);
        if (!request) // a reply, or another use of the group
        {
            return;
        }
        if (request->stop)
        {
            streams_.erase(m.source);
        }
        else if (!request->ids.empty() && request->ids.size() <= options_.max_ids && request->period >= options_.min_period &&
                 (streams_.size() < options_.max_streams || streams_.contains(m.source)))
        {
            start(m.source, *request);
        }
    }

    void start(j1939::Address client, const Request& request)
    {
        auto stream = std::make_unique<Stream>();
        Stream* s = stream.get();
        s->ids = request.ids;
        s->client_name = node_.network().name_at(client); // to see it leave its address
        // The period and the destination are fixed when a PeriodicSender is made: a new Start makes a new one.
        s->sender = std::make_unique<j1939::PeriodicSender>(
            node_, kPgn, request.period, [this, s](std::span<std::uint8_t> out) { return encode(*s, out); },
            j1939::PeriodicSenderOptions{.destination = client, .suppressible = false, .max_length = options_.format.max_reply});
        if (options_.lease.count() > 0)
        {
            s->lease = std::make_unique<j1939::Timer>(node_, j1939::TimerKind::Timeout, [this, client] { streams_.erase(client); });
            s->lease->start_after(options_.lease);
        }
        streams_[client] = std::move(stream); // destroys the client's earlier stream
    }

    std::size_t encode(Stream& s, std::span<std::uint8_t> out)
    {
        readings_.clear();
        for (const std::uint8_t id : s.ids)
        {
            const auto found = table_.find(id);
            readings_.push_back({id, found != table_.end() ? found->second() : std::uint16_t{0xFFFF}});
        }
        bytes_.clear();
        options_.format.write_reply(bytes_, s.sequence++, readings_);
        std::copy_n(bytes_.begin(), (std::min)(bytes_.size(), out.size()), out.begin());
        return (std::min)(bytes_.size(), out.size());
    }

    void drop_gone_clients()
    {
        std::erase_if(streams_, [this](const auto& entry) {
            const auto& name = entry.second->client_name;
            return name && node_.network().address_of(*name) != entry.first;
        });
    }

    j1939::Node& node_;
    PidTable table_;
    StreamerOptions options_;
    std::map<j1939::Address, std::unique_ptr<Stream>> streams_;
    std::vector<Reading> readings_; // scratch
    std::vector<std::uint8_t> bytes_;
    j1939::Subscription commands_;
    j1939::Subscription claims_;
    j1939::Subscription address_;
};

/// The tool's side: node.attach<PidClient>(format). One subscription at a time.
class PidClient
{
public:
    using Callback = std::function<void(const Reply&)>;

    explicit PidClient(j1939::Node& node, Format format = {})
      : node_(node),
        format_(std::move(format)),
        replies_(node, node.on_message(kPgn, [this](const j1939::Message& m) { on_reply(m); }))
    {
    }

    /// Asks `ecu` for `ids` every `period`; `callback` gets each reply, in the step. From a step.
    /// false if the request could not be sent. Repeating it (also to renew a lease) replaces it.
    bool subscribe(j1939::Address ecu, std::vector<std::uint8_t> ids, std::chrono::milliseconds period, Callback callback)
    {
        ecu_ = ecu;
        callback_ = std::move(callback);
        return send({.stop = false, .period = period, .ids = std::move(ids)});
    }

    bool unsubscribe()
    {
        callback_ = nullptr;
        return send({.stop = true});
    }

private:
    bool send(const Request& request)
    {
        bytes_.clear();
        format_.write_request(bytes_, request);
        return node_.send(kPgn, bytes_, ecu_) == j1939::SendStatus::Ok;
    }

    void on_reply(const j1939::Message& m)
    {
        if (!callback_ || m.source != ecu_ || m.destination != node_.address())
        {
            return;
        }
        if (auto reply = format_.read_reply(m.data))
        {
            reply->time = m.time;
            callback_(*reply);
        }
    }

    j1939::Node& node_;
    Format format_;
    j1939::Address ecu_ = j1939::kNullAddress;
    Callback callback_;
    std::vector<std::uint8_t> bytes_;
    j1939::Subscription replies_;
};

} // namespace pid_stream
