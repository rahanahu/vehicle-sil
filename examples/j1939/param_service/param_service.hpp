// SPDX-License-Identifier: MIT
//
// A manufacturer's parameter service over Proprietary A (PGN 0xEF00), written the way an
// application writes a protocol of its own on vehicle-sil: only the public API of
// vehicle_sil::j1939, the Exchange part (commands and their answers) and the typed messages
// generated from param_service_messages.yaml. Nothing of vehicle-sil itself is changed.
//
// The protocol (byte 0 tells the operation; the layouts are in the YAML):
//
//   tool -> ECU                                 ECU -> tool
//   Open {access}                               OpenOk {session, idle_timeout} | Error {busy}
//   Read {session, parameter}                   ReadOk {session, parameter, value} | Error
//   Write {session, parameter, value}           Pending every 100 ms while writing, then WriteOk | Error
//   ReadBlock {session, block}                  BlockData [0x93, session, block, data...] | Error
//   WriteBlock [0x14, session, block, data...]  BlockWritten {session, block, length} | Error
//   Close {session}                             Closed {session}
//
// Blocks longer than 5 bytes travel with the transport protocol. The ECU keeps one session at a
// time. A session ends with Close, after idle_timeout without a command, when the tool's NAME
// leaves its address, or when the ECU's own address changes. A repeated Open or Write of the same
// tool is answered again (the tool may send a command again after a timeout).
#pragma once

#include "vehicle_sil/j1939/exchange.hpp"
#include "vehicle_sil/j1939/toolbox.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace param_service {

namespace j1939 = vehicle_sil::j1939;
using namespace std::chrono_literals;

inline constexpr j1939::Pgn kPgn{0xEF00u};
inline constexpr std::uint8_t kBlockData = 0x93;
inline constexpr std::uint8_t kWriteBlock = 0x14;

// --- the ECU's side ----------------------------------------------------------------------

struct Parameter
{
    std::uint32_t value = 0;
    bool writable = true;
    std::uint32_t min = 0;
    std::uint32_t max = 0xFFFFFFFFu;
};

/// The ECU's data the service reads and writes (the model's, in a real ECU).
struct ParameterTable
{
    std::map<std::uint16_t, Parameter> parameters;
    std::map<std::uint8_t, std::vector<std::uint8_t>> blocks;
    std::size_t max_block = 256;
};

struct ParamServerOptions
{
    std::chrono::milliseconds idle_timeout = 2000ms;
    /// How long a write takes (a flash write): Pending every pending_interval until then.
    vehicle_sil::SimDuration write_time = 300ms;
    vehicle_sil::SimDuration pending_interval = 100ms;
};

struct ParamServerStatistics
{
    std::uint64_t sessions = 0; ///< Sessions opened.
    std::uint64_t expired = 0;  ///< Ended after idle_timeout.
    std::uint64_t lost = 0;     ///< Ended because the tool's or the ECU's address changed.
    std::uint64_t errors = 0;   ///< Error answers sent.
};

/// Answers the service on an ECU's node: node.attach<ParamServer>(table, options).
class ParamServer
{
public:
    ParamServer(j1939::Node& node, ParameterTable& table, ParamServerOptions options = {});

    [[nodiscard]] bool in_session() const noexcept { return open_; }
    [[nodiscard]] j1939::Address client() const noexcept { return client_; }
    [[nodiscard]] const ParamServerStatistics& statistics() const noexcept { return statistics_; }

private:
    void on_command(const j1939::Message& m);
    void on_claim();
    void open(const j1939::Message& m);
    void write(const j1939::Message& m);
    void write_block(const j1939::Message& m);
    void finish_write();
    void end_session();
    /// An Error answer to `to` (the session's tool by default).
    void error(std::uint8_t request, std::uint8_t code, std::optional<j1939::Address> to = std::nullopt);
    template <typename M>
    void reply(const M& message);
    void reply(std::span<const std::uint8_t> bytes);

    j1939::Node& node_;
    ParameterTable& table_;
    ParamServerOptions options_;
    ParamServerStatistics statistics_;
    bool open_ = false;
    bool write_access_ = false;
    j1939::Address client_ = j1939::kNullAddress;
    std::optional<j1939::Name> client_name_;
    std::uint8_t session_ = 0;
    bool writing_ = false;
    std::uint16_t write_parameter_ = 0;
    std::uint32_t write_value_ = 0;
    std::vector<std::uint8_t> out_; ///< Block answers.
    j1939::Subscription commands_;
    j1939::Subscription claims_;
    j1939::Subscription address_;
    j1939::Timer idle_;
    j1939::Timer pending_;
    j1939::Timer written_;
};

// --- the tool's side ---------------------------------------------------------------------

enum class ParamStatus : std::uint8_t
{
    Ok,
    Timeout,    ///< No answer (after the retries).
    ServerLost, ///< The ECU's NAME has no address (left it, or not there).
    NotSent,    ///< The tool's node could not send (no address).
    Busy,       ///< Another tool has the session.
    UnknownParameter,
    ReadOnly,
    OutOfRange,
    Denied,     ///< A write in a read-only session.
    Failed,     ///< Any other error answer.
};

struct ParamClientOptions
{
    vehicle_sil::SimDuration timeout = 200ms; ///< For each answer (Pending restarts it).
    std::uint32_t retries = 1;                ///< Commands sent again after a timeout.
    bool write_access = true;
    std::size_t max_block = 256;
    std::size_t queue_capacity = 8;           ///< Operations waiting their turn.
};

/// Uses the service of the ECU with NAME `server` from a tool's node. Operations run one after
/// the other; each opens a session first if there is none, and opens it again once if the ECU
/// answers that the session ended. The callbacks run in the step; they may queue more operations
/// and may destroy the client.
class ParamClient
{
public:
    using ValueDone = std::function<void(ParamStatus, std::uint32_t value)>;
    using BlockDone = std::function<void(ParamStatus, std::span<const std::uint8_t> data)>;
    using Done = std::function<void(ParamStatus)>;

    ParamClient(j1939::Node& node, j1939::Name server, ParamClientOptions options = {});

    /// false (and no callback) if queue_capacity operations wait.
    bool read(std::uint16_t parameter, ValueDone done);
    bool write(std::uint16_t parameter, std::uint32_t value, Done done);
    bool read_block(std::uint8_t block, BlockDone done);
    bool write_block(std::uint8_t block, std::span<const std::uint8_t> data, Done done);
    /// Ends the session (after the operations queued before).
    bool close(Done done = {});

    [[nodiscard]] bool in_session() const noexcept { return session_.has_value(); }
    [[nodiscard]] std::size_t queued() const noexcept { return queue_.size(); }
    [[nodiscard]] const j1939::ExchangeStatistics& statistics() const noexcept { return exchange_.statistics(); }

private:
    enum class Kind : std::uint8_t
    {
        Read,
        Write,
        ReadBlock,
        WriteBlock,
        Close,
    };
    struct Operation
    {
        Kind kind;
        std::uint16_t id = 0;
        std::uint32_t value = 0;
        std::vector<std::uint8_t> data{};
        ValueDone value_done{};
        BlockDone block_done{};
        Done done{};
        bool reopened = false;
    };

    bool enqueue(Operation operation);
    /// Starts the first operation if none runs.
    void next();
    void open();
    void run();
    void on_answer(const j1939::ExchangeResult& r);
    void complete(ParamStatus status, std::uint32_t value = 0, std::span<const std::uint8_t> data = {});

    j1939::Name server_;
    ParamClientOptions options_;
    std::deque<Operation> queue_;
    bool running_ = false;
    std::optional<std::uint8_t> session_;
    j1939::Exchange exchange_;
};

} // namespace param_service
