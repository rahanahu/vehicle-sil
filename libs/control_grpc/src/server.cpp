// SPDX-License-Identifier: MIT
#include "vehicle_sil/control_grpc/server.hpp"

#include "convert.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/core/simulation_controller.hpp"

#include <grpcpp/grpcpp.h>
#include <silkit/services/orchestration/string_utils.hpp>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <deque>
#include <fstream>
#include <set>
#include <sstream>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::control_grpc {
namespace {

using namespace detail;


class SimulationTimeControl final : public TimeControl
{
public:
    explicit SimulationTimeControl(SimulationController& c) : c_(c) { static_cast<void>(c_.time_state()); }

    TimeView state() const override
    {
        const auto s = c_.time_state();
        TimeView v;
        v.available = true;
        v.now = s.now;
        v.paused = s.paused;
        v.pause_target = s.pause_target;
        v.speed = s.speed;
        const bool finished = c_.runtime().started() && has_finished(PeerStatus{c_.runtime().state()});
        v.running = c_.runtime().started() && !finished;
        v.ended = finished;
        return v;
    }
    void pause(std::optional<SimTime> at) override { at ? c_.pause_at(*at) : c_.pause(); }
    void resume() override { c_.resume(); }
    void run_until(SimTime t) override { c_.run_until(t); }
    void step(SimDuration d) override { c_.step(d); }
    void set_speed(std::optional<double> speed) override { c_.set_speed(speed); }
    std::optional<SimTime> wait_paused(std::chrono::milliseconds timeout) override { return c_.wait_until_paused(timeout); }
    // Asks to stop without waiting; thread safe (the controller's own stop() is neither).
    void stop(const std::string& reason) override { c_.runtime().request_stop(reason); }

    std::vector<std::pair<std::string, std::string>> participants() const override
    {
        std::vector<std::pair<std::string, std::string>> out;
        // SIL Kit can miss a status that arrives while the handler is being registered.
        c_.runtime().refresh_peer_statuses();
        for (const auto& name : c_.required_participants())
        {
            const auto status = c_.runtime().peer_status(name);
            out.emplace_back(name, status.state ? SilKit::Services::Orchestration::to_string(*status.state) : std::string());
        }
        return out;
    }

private:
    using PeerStatus = vehicle_sil::PeerStatus;
    SimulationController& c_;
};

// ---- authentication and authorization ---------------------------------------------------------

/// Who is calling, as far as the server knows.
struct Who
{
    std::string name = "anonymous";
    Role role = Role::Admin;
    std::vector<std::string> participants;
    bool anyone = true; ///< the server has no credentials: every client may do everything
};

bool allows(const Who& who, Role need)
{
    if (who.anyone)
    {
        return true;
    }
    const Role have = who.role;
    if (need == Role::Agent || have == Role::Agent)
    {
        return have == need;
    }
    return static_cast<int>(have) >= static_cast<int>(need);
}

/// Compares in time independent of where the strings differ.
bool same_secret(const std::string& a, const std::string& b)
{
    unsigned diff = a.size() == b.size() ? 0U : 1U;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        diff |= static_cast<unsigned>(static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0));
    }
    return diff == 0;
}

class Authorizer
{
public:
    explicit Authorizer(const SecurityOptions& security) : security_(security) {}

    /// @returns the caller, or nullopt if credentials are required and the call has none that fits.
    [[nodiscard]] std::optional<Who> identify(const grpc::ServerContextBase& context) const
    {
        if (security_.credentials.empty())
        {
            return Who{};
        }
        const auto& metadata = context.client_metadata();
        const auto it = metadata.find("authorization");
        if (it == metadata.end())
        {
            return std::nullopt;
        }
        const std::string value(it->second.data(), it->second.size());
        const std::string prefix = "bearer ";
        if (value.size() <= prefix.size()
            || !std::equal(prefix.begin(), prefix.end(), value.begin(),
                           [](char a, char b) { return a == std::tolower(static_cast<unsigned char>(b)); }))
        {
            return std::nullopt;
        }
        const std::string token = value.substr(prefix.size());
        const Credential* found = nullptr;
        for (const auto& c : security_.credentials)
        {
            if (same_secret(c.token, token)) // no early exit: same time for every credential
            {
                found = &c;
            }
        }
        if (found == nullptr)
        {
            return std::nullopt;
        }
        return Who{found->name, found->role, found->participants, false};
    }

private:
    const SecurityOptions& security_;
};

std::string role_name(Role role)
{
    switch (role)
    {
    case Role::Viewer:
        return "viewer";
    case Role::Operator:
        return "operator";
    case Role::Admin:
        return "admin";
    case Role::Agent:
        return "agent";
    }
    return "?";
}

bool is_loopback(const std::string& listen)
{
    if (listen.rfind("unix:", 0) == 0)
    {
        return true; // a local socket
    }
    const auto colon = listen.rfind(':');
    const std::string host = colon == std::string::npos ? listen : listen.substr(0, colon);
    if (host == "localhost" || host == "[::1]")
    {
        return true;
    }
    // 127.x.y.z, numbers only.
    int parts = 0;
    int first = -1;
    std::size_t pos = 0;
    while (pos <= host.size())
    {
        const auto dot = host.find('.', pos);
        const auto part = host.substr(pos, dot == std::string::npos ? std::string::npos : dot - pos);
        if (part.empty() || part.size() > 3 || part.find_first_not_of("0123456789") != std::string::npos || std::stoi(part) > 255)
        {
            return false;
        }
        if (parts == 0)
        {
            first = std::stoi(part);
        }
        ++parts;
        if (dot == std::string::npos)
        {
            break;
        }
        pos = dot + 1;
    }
    return parts == 4 && first == 127;
}

/// A text for the audit log: no line breaks or other control characters from the caller.
std::string clean(std::string text)
{
    for (auto& c : text)
    {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f)
        {
            c = '?';
        }
    }
    return text;
}

/// At most `max` bytes of `text`, not cut in the middle of a UTF-8 sequence.
std::string cut_utf8(std::string text, std::size_t max)
{
    if (text.size() > max)
    {
        auto cut = max;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }
        text.resize(cut);
    }
    return text;
}

/// The audit log (ServerOptions::audit): cleaned lines of bounded length, and refused calls
/// thinned out, since anyone who reaches the port can make those (a flood must not fill the disk
/// or slow the other calls down).
class AuditLog
{
public:
    static constexpr std::size_t kMaxLine = 1000;
    static constexpr int kRefusedPerSecond = 20;

    explicit AuditLog(std::function<void(const std::string&)> sink) : sink_(std::move(sink)) {}
    AuditLog(const AuditLog&) = delete;
    AuditLog& operator=(const AuditLog&) = delete;

    void operator()(std::string text) const
    {
        if (sink_)
        {
            write(std::move(text));
        }
    }

    void refused(std::string text) const
    {
        if (!sink_)
        {
            return;
        }
        std::uint64_t left_out = 0;
        {
            const std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            if (now - window_ >= std::chrono::seconds(1))
            {
                window_ = now;
                in_window_ = 0;
                left_out = std::exchange(left_out_, 0);
            }
            if (in_window_ >= kRefusedPerSecond)
            {
                ++left_out_;
                return;
            }
            ++in_window_;
        }
        if (left_out > 0)
        {
            write("(" + std::to_string(left_out) + " more refused calls not logged)");
        }
        write(std::move(text));
    }

private:
    void write(std::string text) const
    {
        text = clean(std::move(text));
        if (text.size() > kMaxLine)
        {
            text = cut_utf8(std::move(text), kMaxLine) + "...";
        }
        try
        {
            sink_(text);
        }
        catch (const std::exception&)
        {
            // A failing log must not end a call (or a gRPC callback) halfway.
        }
    }

    std::function<void(const std::string&)> sink_;
    mutable std::mutex mutex_;
    mutable std::chrono::steady_clock::time_point window_{};
    mutable int in_window_ = 0;
    mutable std::uint64_t left_out_ = 0;
};

/// The live agent streams, so that the server can drop them (ControlServer::disconnect_agents()).
struct StreamRegistry
{
    struct Handle
    {
        std::recursive_mutex mutex;
        grpc::CallbackServerContext* context = nullptr; // nullptr once the stream is done
    };
    std::mutex mutex;
    std::vector<std::weak_ptr<Handle>> handles;

    std::shared_ptr<Handle> add(grpc::CallbackServerContext* context)
    {
        auto handle = std::make_shared<Handle>();
        handle->context = context;
        const std::lock_guard lock(mutex);
        std::erase_if(handles, [](const std::weak_ptr<Handle>& h) { return h.expired(); });
        handles.push_back(handle);
        return handle;
    }

    void cancel_all()
    {
        std::vector<std::shared_ptr<Handle>> live;
        {
            const std::lock_guard lock(mutex);
            for (const auto& h : handles)
            {
                if (auto p = h.lock())
                {
                    live.push_back(std::move(p));
                }
            }
        }
        for (const auto& h : live)
        {
            const std::lock_guard lock(h->mutex);
            if (h->context != nullptr)
            {
                h->context->TryCancel();
            }
        }
    }
};

constexpr std::uint64_t kMaxAgentPending = 65536;
/// Waits are cut into slices this long to see cancellation and shutdown.
constexpr auto kWaitSlice = std::chrono::milliseconds(200);

// ---- the agent stream ------------------------------------------------------------------------

/// One agent connection: a bidirectional stream. Owned by gRPC (deleted in OnDone).
class AgentStream final : public grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>
{
public:
    /// The hub's link to this stream. Outlives the stream (the hub may hold it); once the stream
    /// is done, send() refuses.
    struct Link final : AgentLink
    {
        std::mutex mutex;
        AgentStream* stream = nullptr; // guarded by mutex; nullptr once done
        bool send(const Command& command) override
        {
            const std::lock_guard lock(mutex);
            return stream != nullptr && stream->queue(command);
        }
        void close() override
        {
            // Finished with ABORTED, which the agent takes as a refusal (it waits reconnect_max:
            // two live processes with one name do not take the connection from each other many
            // times a second). Not under this mutex: OnDone() locks the handle and then this
            // mutex. While the handle's mutex is held and its context is set, OnDone() has not
            // begun, so the stream is alive.
            AgentStream* s = nullptr;
            std::shared_ptr<StreamRegistry::Handle> handle;
            {
                const std::lock_guard lock(mutex);
                if (stream == nullptr)
                {
                    return;
                }
                s = stream;
                handle = stream->handle_;
            }
            const std::lock_guard lock(handle->mutex);
            if (handle->context == nullptr)
            {
                return;
            }
            // Not thinned with the refused calls: a flood of those must not hide a takeover.
            s->audit_(s->who_.name + "@" + s->peer_ + " agent: replaced by another connection of the same participant");
            s->finish(grpc::Status(grpc::StatusCode::ABORTED, "replaced by another connection of the same participant"));
        }
    };

    AgentStream(Hub& hub, Who who, std::string peer, const AuditLog& audit,
                std::shared_ptr<StreamRegistry::Handle> handle) :
      hub_(hub),
      link_(std::make_shared<Link>()),
      who_(std::move(who)),
      peer_(std::move(peer)),
      audit_(audit),
      handle_(std::move(handle))
    {
        link_->stream = this;
        StartRead(&in_);
    }

    void OnReadDone(bool ok) override
    {
        if (!ok)
        {
            finish(grpc::Status::OK);
            return;
        }
        if (!dispatch())
        {
            return;
        }
        {
            // Another thread may have finished the stream meanwhile (close(), OnCancel()).
            const std::lock_guard lock(write_mutex_);
            if (finished_)
            {
                return;
            }
        }
        in_.Clear();
        StartRead(&in_);
    }

    void OnWriteDone(bool ok) override
    {
        const std::lock_guard lock(write_mutex_);
        out_.pop_front();
        writing_ = false;
        if (!ok)
        {
            broken_ = true;
            return;
        }
        start_write();
    }

    void OnDone() override
    {
        {
            const std::lock_guard lock(handle_->mutex);
            handle_->context = nullptr;
        }
        {
            const std::lock_guard lock(link_->mutex);
            link_->stream = nullptr;
        }
        if (connection_ != 0)
        {
            hub_.agent_disconnected(connection_);
        }
        delete this;
    }

    void OnCancel() override { finish(grpc::Status::CANCELLED); }

private:
    /// Called with link_->mutex held.
    bool queue(const Command& command)
    {
        const std::lock_guard lock(write_mutex_);
        if (broken_ || finished_)
        {
            return false;
        }
        v1::ServerMessage message;
        to_proto(command, *message.mutable_command());
        out_.push_back(std::move(message));
        start_write();
        return true;
    }

    /// write_mutex_ held.
    void start_write()
    {
        if (!writing_ && !out_.empty() && !finished_)
        {
            writing_ = true;
            StartWrite(&out_.front());
        }
    }

    void finish(grpc::Status status)
    {
        {
            const std::lock_guard lock(write_mutex_);
            if (std::exchange(finished_, true))
            {
                return;
            }
        }
        Finish(std::move(status));
    }

    /// @returns false if the stream was finished.
    bool dispatch()
    {
        if (connection_ == 0)
        {
            const auto refuse = [&](grpc::StatusCode code, const std::string& why) {
                audit_.refused(who_.name + "@" + peer_ + " agent "
                               + (in_.has_hello() ? in_.hello().participant() : std::string("?")) + ": refused (" + why + ")");
                finish(grpc::Status(code, why));
                return false;
            };
            if (!in_.has_hello() || in_.hello().participant().empty() || in_.hello().max_pending() == 0
                || in_.hello().max_pending() > kMaxAgentPending)
            {
                return refuse(grpc::StatusCode::INVALID_ARGUMENT, "the first message must be a valid hello");
            }
            const auto& h = in_.hello();
            {
                std::set<std::string> names;
                for (const auto& v : h.schema())
                {
                    if (v.name().empty() || !names.insert(v.name()).second)
                    {
                        return refuse(grpc::StatusCode::INVALID_ARGUMENT, "the schema has an empty or repeated name");
                    }
                }
            }
            // An agent holds at most max_pending queued commands and as many recent results.
            if (static_cast<std::uint64_t>(h.open_seqs_size()) > 2 * h.max_pending())
            {
                return refuse(grpc::StatusCode::INVALID_ARGUMENT, "more open_seqs than the agent can hold");
            }
            if (!who_.participants.empty()
                && std::find(who_.participants.begin(), who_.participants.end(), h.participant()) == who_.participants.end())
            {
                return refuse(grpc::StatusCode::PERMISSION_DENIED, "this credential may not connect as " + h.participant());
            }
            audit_(who_.name + "@" + peer_ + " agent " + h.participant() + " (" + h.incarnation() + "): connected");
            PeerHello hello;
            hello.participant = h.participant();
            hello.incarnation = h.incarnation();
            for (const auto& v : h.schema())
            {
                hello.schema.push_back(from_proto(v));
            }
            hello.max_pending = static_cast<std::size_t>(h.max_pending());
            if (h.has_open_seqs())
            {
                hello.open_seqs = std::vector<std::uint64_t>(h.open_seqs().begin(), h.open_seqs().end());
            }
            {
                // The welcome goes into the queue before the hub knows the link, so it is written
                // before any command (the hub may send one as soon as agent_connected() swapped
                // the link in).
                v1::ServerMessage welcome;
                welcome.mutable_welcome();
                const std::lock_guard lock(write_mutex_);
                out_.push_back(std::move(welcome));
                start_write();
            }
            connection_ = hub_.agent_connected(std::move(hello), link_);
            return true;
        }
        switch (in_.kind_case())
        {
        case v1::AgentMessage::kSubmitted:
        {
            const auto& s = in_.submitted();
            SubmitResult r;
            r.status = s.status() == v1::SUBMIT_STATUS_QUEUED ? SubmitStatus::Queued
                       : s.status() == v1::SUBMIT_STATUS_BUSY ? SubmitStatus::Busy
                                                              : SubmitStatus::Invalid; // also UNSPECIFIED and unknown
            r.reason = s.reason();
            hub_.agent_submitted(connection_, s.seq(), r);
            break;
        }
        case v1::AgentMessage::kResult:
            if (const auto result = from_proto(in_.result()))
            {
                hub_.agent_result(connection_, *result);
            }
            break; // a status this server does not know: the operation stays open
        case v1::AgentMessage::kValues:
            hub_.agent_values(connection_, from_proto(in_.values()));
            break;
        case v1::AgentMessage::kEvent:
            hub_.agent_events(connection_, {from_proto(in_.event())});
            break;
        case v1::AgentMessage::kStatus:
            hub_.agent_status(connection_, control::PeerStatus{in_.status().failed(), in_.status().error()});
            break;
        default:
            break; // a second hello or an unknown message: ignored
        }
        return true;
    }

    Hub& hub_;
    std::shared_ptr<Link> link_;
    Who who_;
    std::string peer_;
    const AuditLog& audit_;
    std::shared_ptr<StreamRegistry::Handle> handle_;
    v1::AgentMessage in_;
    ConnectionId connection_ = 0;
    std::mutex write_mutex_;
    std::deque<v1::ServerMessage> out_; // bounded by the hub: at most max_pending commands are open per peer
    bool writing_ = false;
    bool broken_ = false;
    bool finished_ = false;
};

/// A stream refused before it began.
class RefusedStream final : public grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>
{
public:
    explicit RefusedStream(grpc::Status status) { Finish(std::move(status)); }
    void OnDone() override { delete this; }
};

class AgentServiceImpl final : public v1::AgentService::CallbackService
{
public:
    AgentServiceImpl(Hub& hub, const Authorizer& auth, const AuditLog& audit, StreamRegistry& streams) :
      hub_(hub), auth_(auth), audit_(audit), streams_(streams)
    {
    }
    grpc::ServerBidiReactor<v1::AgentMessage, v1::ServerMessage>* Connect(grpc::CallbackServerContext* context) override
    {
        const auto who = auth_.identify(*context);
        if (!who || !allows(*who, Role::Agent))
        {
            audit_.refused((who ? who->name : std::string("unauthenticated")) + "@" + context->peer() + " agent: refused");
            return new RefusedStream(grpc::Status(who ? grpc::StatusCode::PERMISSION_DENIED : grpc::StatusCode::UNAUTHENTICATED,
                                                  who ? "this credential may not connect an agent" : "a valid agent token is required"));
        }
        return new AgentStream(hub_, *who, context->peer(), audit_, streams_.add(context));
    }

private:
    Hub& hub_;
    const Authorizer& auth_;
    const AuditLog& audit_;
    StreamRegistry& streams_;
};

// ---- the client service ------------------------------------------------------------------------

grpc::Status error_status(const std::exception& e)
{
    const bool argument = dynamic_cast<const std::invalid_argument*>(&e) != nullptr;
    return grpc::Status(argument ? grpc::StatusCode::INVALID_ARGUMENT : grpc::StatusCode::FAILED_PRECONDITION, e.what());
}

void time_to_proto(const TimeView& in, v1::TimeState& out)
{
    out.set_available(in.available);
    out.set_now_ns(ns_of(in.now));
    out.set_paused(in.paused);
    if (in.pause_target)
    {
        out.set_pause_target_ns(ns_of(*in.pause_target));
    }
    if (in.speed)
    {
        out.set_speed(*in.speed);
    }
    out.set_running(in.running);
}

std::string peer_of(const grpc::ServerContext& context)
{
    return context.peer();
}

class ControlServiceImpl final : public v1::ControlService::Service
{
public:
    ControlServiceImpl(Hub& hub, const ServerOptions& options, const std::atomic<bool>& stopping, const Authorizer& auth,
                       const AuditLog& audit) :
      hub_(hub), options_(options), stopping_(stopping), auth_(auth), audit_(audit)
    {
    }

    /// Counts a call that waits (holds a server thread); empty if too many wait already.
    class WaitingCall
    {
    public:
        explicit WaitingCall(std::atomic<std::size_t>& count) : count_(&count) {}
        WaitingCall(const WaitingCall&) = delete;
        WaitingCall& operator=(const WaitingCall&) = delete;
        ~WaitingCall() { count_->fetch_sub(1, std::memory_order_acq_rel); }

    private:
        std::atomic<std::size_t>* count_;
    };

    std::optional<WaitingCall> enter_waiting()
    {
        if (waiting_.fetch_add(1, std::memory_order_acq_rel) >= options_.max_waiting_calls)
        {
            waiting_.fetch_sub(1, std::memory_order_acq_rel);
            return std::nullopt;
        }
        return std::optional<WaitingCall>(std::in_place, waiting_);
    }

    static grpc::Status too_many_waiting()
    {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED, "too many calls wait at the same time (Watch, WaitOperation, WaitPaused)");
    }

    /// @returns a refusal, or nullopt and `who` when the caller has at least `need`.
    std::optional<grpc::Status> authorize(grpc::ServerContext* context, Role need, Who& who, const char* what)
    {
        const auto found = auth_.identify(*context);
        if (found && allows(*found, need))
        {
            who = *found;
            return std::nullopt;
        }
        audit_.refused((found ? found->name : std::string("unauthenticated")) + "@" + context->peer() + " " + what + ": refused");
        if (!found)
        {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "a valid token is required");
        }
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "the role " + role_name(found->role) + " may not " + what);
    }

    void audit(const std::string& name, const grpc::ServerContext& context, const std::string& what, const std::string& outcome)
    {
        audit_(name + "@" + context.peer() + " " + what + ": " + outcome);
    }

    grpc::Status Ping(grpc::ServerContext* context, const v1::PingRequest* request, v1::PingReply* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "ping"))
        {
            return *denied;
        }
        reply->set_text(request->text());
        reply->set_server_version("vehicle-sil control v1");
        return grpc::Status::OK;
    }

    grpc::Status ListPeers(grpc::ServerContext* context, const v1::ListPeersRequest*, v1::ListPeersReply* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "list peers"))
        {
            return *denied;
        }
        // The agents that connected, joined with the participants SIL Kit knows (with a time
        // control: the simulation's participants on every PC, also those without an agent).
        std::vector<std::pair<std::string, std::string>> simulation;
        if (options_.time != nullptr)
        {
            try
            {
                simulation = options_.time->participants();
            }
            catch (const std::exception&)
            {
            }
        }
        const auto state_of = [&](const std::string& name) {
            for (const auto& [n, state] : simulation)
            {
                if (n == name)
                {
                    return state;
                }
            }
            return std::string();
        };
        for (const auto& p : hub_.peers())
        {
            auto* out = reply->add_peers();
            detail::to_proto(p, *out);
            out->set_has_agent(true);
            out->set_simulation_state(state_of(p.participant));
        }
        for (const auto& [name, state] : simulation)
        {
            const auto known = std::any_of(reply->peers().begin(), reply->peers().end(),
                                           [&](const v1::PeerInfo& p) { return p.participant() == name; });
            if (!known)
            {
                auto* out = reply->add_peers();
                out->set_participant(name);
                out->set_has_agent(false);
                out->set_simulation_state(state);
            }
        }
        return grpc::Status::OK;
    }

    grpc::Status GetValues(grpc::ServerContext* context, const v1::GetValuesRequest* request, v1::GetValuesReply* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "read values"))
        {
            return *denied;
        }
        for (const auto& p : hub_.peers())
        {
            if (p.participant == request->peer())
            {
                if (const auto latest = hub_.latest(p.participant))
                {
                    reply->set_has_values(true);
                    reply->set_incarnation(p.incarnation);
                    detail::to_proto(*latest, *reply->mutable_snapshot());
                }
                return grpc::Status::OK;
            }
        }
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "no peer " + request->peer());
    }

    grpc::Status Write(grpc::ServerContext* context, const v1::WriteRequest* request, v1::WriteReply* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "write"))
        {
            return *denied;
        }
        if (static_cast<std::size_t>(request->writes_size()) > options_.max_writes_per_request)
        {
            audit(who.name, *context, "write " + std::to_string(request->writes_size()) + " values", "refused (too many)");
            return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                "at most " + std::to_string(options_.max_writes_per_request) + " writes in one request");
        }
        OperationRequest r;
        for (const auto& w : request->writes())
        {
            Value value;
            if (!from_proto(w.value(), value))
            {
                return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "a write without a value");
            }
            r.writes.push_back({w.peer(), w.name(), value});
        }
        if (request->has_at_ns())
        {
            r.at = time_of(request->at_ns());
        }
        r.late = late_of(request->late());
        r.client = who.name + "@" + peer_of(*context);
        std::string what = "write";
        constexpr std::size_t kNamed = 16; // the audit line names the first ones
        for (std::size_t i = 0; i < r.writes.size() && i < kNamed; ++i)
        {
            what += " " + r.writes[i].peer + "/" + r.writes[i].name;
        }
        if (r.writes.size() > kNamed)
        {
            what += " (+" + std::to_string(r.writes.size() - kNamed) + " more)";
        }
        try
        {
            const auto seq = hub_.write(std::move(r));
            reply->set_seq(seq);
            audit(who.name, *context, what, "seq " + std::to_string(seq));
        }
        catch (const std::exception& e)
        {
            audit(who.name, *context, what, e.what());
            return error_status(e);
        }
        return grpc::Status::OK;
    }

    grpc::Status GetOperation(grpc::ServerContext* context, const v1::GetOperationRequest* request, v1::Operation* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "read operations"))
        {
            return *denied;
        }
        const auto op = hub_.operation(request->seq());
        if (!op)
        {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "no operation " + std::to_string(request->seq()));
        }
        detail::to_proto(*op, *reply);
        return grpc::Status::OK;
    }

    grpc::Status WaitOperation(grpc::ServerContext* context, const v1::WaitOperationRequest* request, v1::Operation* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "wait for operations"))
        {
            return *denied;
        }
        const auto waiting = enter_waiting();
        if (!waiting)
        {
            return too_many_waiting();
        }
        const auto deadline = std::chrono::steady_clock::now()
                              + std::min(options_.max_wait, std::chrono::milliseconds(request->timeout_ms()));
        const bool queued = request->until() == v1::WAIT_FOR_QUEUED;
        // In short slices: a cancelled call or a shutdown ends the wait.
        while (true)
        {
            const auto slice = std::min<std::chrono::steady_clock::duration>(
                kWaitSlice, std::max<std::chrono::steady_clock::duration>(deadline - std::chrono::steady_clock::now(), {}));
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(slice);
            const auto op = queued ? hub_.wait_queued(request->seq(), ms) : hub_.wait_done(request->seq(), ms);
            if (op)
            {
                detail::to_proto(*op, *reply);
                return grpc::Status::OK;
            }
            if (!hub_.operation(request->seq()))
            {
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "no operation " + std::to_string(request->seq()));
            }
            if (context->IsCancelled() || stopping_.load())
            {
                return grpc::Status(grpc::StatusCode::CANCELLED, "the wait was cancelled");
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "the operation did not get there in time");
            }
        }
    }

    grpc::Status Watch(grpc::ServerContext* context, const v1::WatchRequest* request, grpc::ServerWriter<v1::Update>* writer) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "watch"))
        {
            return *denied;
        }
        const auto waiting = enter_waiting();
        if (!waiting)
        {
            return too_many_waiting();
        }
        SubscriptionOptions o;
        o.peers.assign(request->peers().begin(), request->peers().end());
        o.values = request->values();
        o.events = request->events();
        o.operations = request->operations();
        o.capacity = std::min<std::size_t>(request->capacity() == 0 ? options_.default_watch_capacity : request->capacity(),
                                           options_.max_watch_capacity);
        const bool watch_time = request->time() && options_.time != nullptr;
        const auto sub = hub_.subscribe(o);
        std::optional<std::string> sent_time; // the last time state sent (serialized)
        const auto send_time = [&]() {
            if (!watch_time)
            {
                return true;
            }
            v1::Update out;
            try
            {
                time_to_proto(options_.time->state(), *out.mutable_time());
            }
            catch (const std::exception&)
            {
                return true;
            }
            auto text = out.time().SerializeAsString();
            if (sent_time == text)
            {
                return true;
            }
            sent_time = std::move(text);
            out.set_dropped(sub->dropped());
            return writer->Write(out);
        };
        bool initial = true;
        while (!context->IsCancelled() && !stopping_.load())
        {
            // The first poll takes everything the subscription starts with (peers, latest values).
            for (auto& update : sub->poll(initial ? 0 : 64, initial ? std::chrono::milliseconds(0) : std::chrono::milliseconds(100)))
            {
                v1::Update out;
                if (const auto* p = std::get_if<PeerUpdate>(&update))
                {
                    if (p->removed)
                    {
                        out.set_removed_peer(p->peer.participant);
                    }
                    else
                    {
                        detail::to_proto(p->peer, *out.mutable_peer());
                    }
                }
                else if (const auto* v = std::get_if<ValuesUpdate>(&update))
                {
                    auto* pv = out.mutable_values();
                    pv->set_peer(v->peer);
                    pv->set_incarnation(v->incarnation);
                    detail::to_proto(*v->snapshot, *pv->mutable_snapshot());
                }
                else if (const auto* e = std::get_if<EventUpdate>(&update))
                {
                    auto* pe = out.mutable_event();
                    pe->set_peer(e->peer);
                    pe->set_incarnation(e->incarnation);
                    detail::to_proto(e->event, *pe->mutable_event());
                }
                else
                {
                    detail::to_proto(std::get<OperationUpdate>(update).operation, *out.mutable_operation());
                }
                out.set_dropped(sub->dropped());
                if (!writer->Write(out))
                {
                    sub->close();
                    return grpc::Status::OK; // the client went away
                }
            }
            if (!send_time())
            {
                break;
            }
            if (std::exchange(initial, false))
            {
                v1::Update done;
                done.set_initial_done(true);
                if (!writer->Write(done))
                {
                    break;
                }
            }
        }
        sub->close();
        return grpc::Status::OK;
    }

    grpc::Status ForgetPeer(grpc::ServerContext* context, const v1::ForgetPeerRequest* request, v1::ForgetPeerReply*) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "forget a peer"))
        {
            return *denied;
        }
        const bool forgotten = hub_.forget_peer(request->peer());
        audit(who.name, *context, "forget " + request->peer(), forgotten ? "ok" : "refused (unknown or connected)");
        if (!forgotten)
        {
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "no disconnected peer " + request->peer());
        }
        return grpc::Status::OK;
    }

    // ---- time ----------------------------------------------------------------------------

    template <typename F>
    grpc::Status with_time(v1::TimeState* reply, F&& f, const Who* who = nullptr, grpc::ServerContext* context = nullptr,
                           const char* what = nullptr)
    {
        auto status = with_time_impl(reply, std::forward<F>(f));
        if (who != nullptr && context != nullptr && what != nullptr)
        {
            audit(who->name, *context, what, status.ok() ? "ok" : status.error_message());
        }
        return status;
    }

    template <typename F>
    grpc::Status with_time_impl(v1::TimeState* reply, F&& f)
    {
        if (options_.time == nullptr)
        {
            if (reply != nullptr)
            {
                reply->set_available(false);
            }
            return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "this server does not control the time");
        }
        try
        {
            f(*options_.time);
            time_to_proto(options_.time->state(), *reply);
        }
        catch (const std::exception& e)
        {
            return error_status(e);
        }
        return grpc::Status::OK;
    }

    grpc::Status GetTime(grpc::ServerContext* context, const v1::GetTimeRequest*, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "read the time"))
        {
            return *denied;
        }
        if (options_.time == nullptr)
        {
            reply->set_available(false);
            return grpc::Status::OK;
        }
        return with_time(reply, [](TimeControl&) {});
    }
    grpc::Status Pause(grpc::ServerContext* context, const v1::PauseRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "pause"))
        {
            return *denied;
        }
        return with_time(reply, [&](TimeControl& t) { t.pause(r->has_at_ns() ? std::optional(time_of(r->at_ns())) : std::nullopt); }, &who, context, "pause");
    }
    grpc::Status Resume(grpc::ServerContext* context, const v1::ResumeRequest*, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "resume"))
        {
            return *denied;
        }
        return with_time(reply, [](TimeControl& t) { t.resume(); }, &who, context, "resume");
    }
    grpc::Status RunUntil(grpc::ServerContext* context, const v1::RunUntilRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "run until"))
        {
            return *denied;
        }
        return with_time(reply, [&](TimeControl& t) { t.run_until(time_of(r->at_ns())); }, &who, context, "run until");
    }
    grpc::Status Step(grpc::ServerContext* context, const v1::StepRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "step"))
        {
            return *denied;
        }
        return with_time(reply, [&](TimeControl& t) { t.step(std::chrono::nanoseconds(r->duration_ns())); }, &who, context, "step");
    }
    grpc::Status SetSpeed(grpc::ServerContext* context, const v1::SetSpeedRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Operator, who, "set the speed"))
        {
            return *denied;
        }
        return with_time(reply, [&](TimeControl& t) { t.set_speed(r->has_speed() ? std::optional(r->speed()) : std::nullopt); }, &who, context, "set the speed");
    }
    grpc::Status WaitPaused(grpc::ServerContext* context, const v1::WaitPausedRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Viewer, who, "wait for a pause"))
        {
            return *denied;
        }
        const auto waiting = enter_waiting();
        if (!waiting)
        {
            return too_many_waiting();
        }
        const auto deadline = std::chrono::steady_clock::now() + std::min(options_.max_wait, std::chrono::milliseconds(r->timeout_ms()));
        while (true)
        {
            bool reached = false;
            bool ended = false;
            const auto slice = std::chrono::duration_cast<std::chrono::milliseconds>(std::min<std::chrono::steady_clock::duration>(
                kWaitSlice, std::max<std::chrono::steady_clock::duration>(deadline - std::chrono::steady_clock::now(), {})));
            auto status = with_time(reply, [&](TimeControl& t) {
                reached = t.wait_paused(slice).has_value();
                ended = !reached && t.state().ended;
            });
            if (!status.ok() || reached)
            {
                return status;
            }
            if (ended)
            {
                // It will not stand still again (and wait_paused() returns at once: no busy loop).
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "the simulation has ended");
            }
            if (context->IsCancelled() || stopping_.load())
            {
                return grpc::Status(grpc::StatusCode::CANCELLED, "the wait was cancelled");
            }
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return grpc::Status(grpc::StatusCode::DEADLINE_EXCEEDED, "the simulation did not stand still in time");
            }
        }
    }
    grpc::Status Stop(grpc::ServerContext* context, const v1::StopRequest* r, v1::TimeState* reply) override
    {
        Who who;
        if (auto denied = authorize(context, Role::Admin, who, "stop"))
        {
            return *denied;
        }
        // The reason goes to every participant and into SIL Kit's logs: cleaned and bounded.
        constexpr std::size_t kMaxReason = 200;
        auto reason = clean(cut_utf8(r->reason(), kMaxReason));
        if (reason.empty())
        {
            reason = "stopped by a client";
        }
        const auto what = "stop (" + reason + ")";
        return with_time(reply, [&](TimeControl& t) { t.stop(reason); }, &who, context, what.c_str());
    }

private:
    Hub& hub_;
    const ServerOptions& options_;
    const std::atomic<bool>& stopping_;
    const Authorizer& auth_;
    const AuditLog& audit_;
    std::atomic<std::size_t> waiting_{0};
};

} // namespace

std::vector<Credential> load_credentials(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        throw std::runtime_error("cannot read the credentials file " + path);
    }
    std::vector<Credential> out;
    std::string line;
    int number = 0;
    const auto fail = [&](const std::string& why) {
        return std::runtime_error(path + ":" + std::to_string(number) + ": " + why);
    };
    while (std::getline(in, line))
    {
        ++number;
        std::istringstream words(line);
        std::vector<std::string> fields;
        for (std::string w; words >> w;)
        {
            if (w[0] == '#')
            {
                break; // a comment: a word that starts with '#'
            }
            fields.push_back(w);
        }
        if (fields.empty())
        {
            continue;
        }
        if (fields.size() < 3)
        {
            throw fail("expected <role> <name> <token> [participant,...]");
        }
        Credential c;
        const auto& role = fields[0];
        if (role == "viewer")
        {
            c.role = Role::Viewer;
        }
        else if (role == "operator")
        {
            c.role = Role::Operator;
        }
        else if (role == "admin")
        {
            c.role = Role::Admin;
        }
        else if (role == "agent")
        {
            c.role = Role::Agent;
        }
        else
        {
            throw fail("unknown role " + role);
        }
        c.name = fields[1];
        c.token = fields[2];
        if (c.token.size() < 16)
        {
            throw fail("a token needs at least 16 characters");
        }
        // The participants: the rest of the line, separated by commas and/or spaces.
        for (std::size_t i = 3; i < fields.size(); ++i)
        {
            std::istringstream list(fields[i]);
            for (std::string p; std::getline(list, p, ',');)
            {
                if (!p.empty())
                {
                    c.participants.push_back(p);
                }
            }
        }
        if (!c.participants.empty() && c.role != Role::Agent)
        {
            throw fail("only an agent credential names participants");
        }
        for (const auto& other : out)
        {
            if (other.token == c.token)
            {
                throw fail("the token of " + c.name + " is used by " + other.name + " already");
            }
        }
        out.push_back(std::move(c));
    }
    return out;
}

std::unique_ptr<TimeControl> time_control_of(SimulationController& controller)
{
    return std::make_unique<SimulationTimeControl>(controller);
}

namespace {

/// The checks of load_credentials() also for credentials given directly, and the other options.
ServerOptions check(ServerOptions o)
{
    const auto& sec = o.security;
    if (sec.certificate_chain_pem.empty() != sec.private_key_pem.empty())
    {
        throw std::invalid_argument("vehicle_sil control server: TLS needs both the certificate chain and the private key");
    }
    for (std::size_t i = 0; i < sec.credentials.size(); ++i)
    {
        const auto& c = sec.credentials[i];
        if (c.token.size() < 16)
        {
            throw std::invalid_argument("vehicle_sil control server: the token of " + c.name + " has fewer than 16 characters");
        }
        if (!c.participants.empty() && c.role != Role::Agent)
        {
            throw std::invalid_argument("vehicle_sil control server: only an agent credential names participants (" + c.name + ")");
        }
        for (std::size_t j = 0; j < i; ++j)
        {
            if (sec.credentials[j].token == c.token)
            {
                throw std::invalid_argument("vehicle_sil control server: " + c.name + " and " + sec.credentials[j].name
                                            + " have the same token");
            }
        }
    }
    if (o.max_waiting_calls == 0 || o.max_writes_per_request == 0)
    {
        throw std::invalid_argument("vehicle_sil control server: max_waiting_calls and max_writes_per_request must be > 0");
    }
    return o;
}

} // namespace

struct ControlServer::Impl
{
    ServerOptions options;
    AuditLog audit;
    Hub hub;
    std::atomic<bool> stopping{false};
    Authorizer auth;
    StreamRegistry streams;
    AgentServiceImpl agents;
    ControlServiceImpl clients;
    std::unique_ptr<grpc::Server> server;
    int port = 0;
    std::mutex shutdown_mutex;
    bool shut_down = false;

    explicit Impl(ServerOptions o) :
      options(check(std::move(o))),
      audit(options.audit),
      hub(options.hub),
      auth(options.security),
      agents(hub, auth, audit, streams),
      clients(hub, options, stopping, auth, audit)
    {
        const auto& sec = options.security;
        const bool tls = !sec.certificate_chain_pem.empty() && !sec.private_key_pem.empty();
        if (!is_loopback(options.listen) && !sec.allow_insecure && (!tls || sec.credentials.empty()))
        {
            throw std::invalid_argument("vehicle_sil control server: listening on " + options.listen
                                        + " (not the loopback) needs TLS and credentials, or allow_insecure");
        }
        std::shared_ptr<grpc::ServerCredentials> credentials;
        if (tls)
        {
            grpc::SslServerCredentialsOptions ssl;
            ssl.pem_key_cert_pairs.push_back({sec.private_key_pem, sec.certificate_chain_pem});
            credentials = grpc::SslServerCredentials(ssl);
        }
        else
        {
            credentials = grpc::InsecureServerCredentials();
        }
        grpc::ServerBuilder builder;
        // Keepalive: a link that went silent (a NAT or firewall dropped it) is found within
        // seconds, also while the simulation stands still and nothing flows.
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS, 5000);
        builder.AddChannelArgument(GRPC_ARG_HTTP2_MAX_PINGS_WITHOUT_DATA, 0);
        builder.AddListeningPort(options.listen, credentials, &port);
        builder.RegisterService(&agents);
        builder.RegisterService(&clients);
        server = builder.BuildAndStart();
        if (!server || port == 0)
        {
            throw std::runtime_error("vehicle_sil control server: cannot listen on " + options.listen);
        }
    }
};

ControlServer::ControlServer(ServerOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}

ControlServer::~ControlServer()
{
    shutdown();
}

int ControlServer::port() const noexcept
{
    return impl_->port;
}

control::Hub& ControlServer::hub() noexcept
{
    return impl_->hub;
}

void ControlServer::disconnect_agents()
{
    impl_->streams.cancel_all();
}

void ControlServer::shutdown()
{
    const std::lock_guard lock(impl_->shutdown_mutex);
    if (std::exchange(impl_->shut_down, true))
    {
        return;
    }
    impl_->stopping = true;
    impl_->server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
    impl_->server->Wait();
}

} // namespace vehicle_sil::control_grpc
