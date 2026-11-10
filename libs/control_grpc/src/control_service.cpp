// SPDX-License-Identifier: MIT
#include "control_service.hpp"

#include "convert.hpp"
#include "vehicle_sil/control/v1/control.grpc.pb.h"
#include "vehicle_sil/control_grpc/simulation_monitor.hpp"

#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string_view>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::control_grpc::detail {
namespace {

/// Snapshots of the simulation a Watch holds before it sends them; beyond, the oldest are dropped.
constexpr std::size_t kMaxPendingSnapshots = 64;
/// Waits are cut into slices this long to see cancellation and shutdown.
constexpr auto kWaitSlice = std::chrono::milliseconds(200);

// ---- the client service ------------------------------------------------------------------------

grpc::Status error_status(const std::exception& e)
{
    const bool argument = dynamic_cast<const std::invalid_argument*>(&e) != nullptr;
    return grpc::Status(argument ? grpc::StatusCode::INVALID_ARGUMENT : grpc::StatusCode::FAILED_PRECONDITION, e.what());
}

std::string peer_of(const grpc::ServerContext& context)
{
    return context.peer();
}

class ControlServiceImpl final : public v1::ControlService::Service
{
public:
    ControlServiceImpl(Hub& hub, const ServerOptions& options, SimulationMonitor* monitor, const std::atomic<bool>& stopping,
                       const Authorizer& auth, const AuditLog& audit) :
      hub_(hub), options_(options), monitor_(monitor), stopping_(stopping), auth_(auth), audit_(audit)
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

    /// An allowed call: who makes it, and the rule of the authorization table that allows it.
    struct Call
    {
        Who who;
        const RpcRule* rule;
        grpc::ServerContext* context;
    };

    /// The first line of every RPC: runs `body` with the caller if the authorization table lets
    /// them call `method`, or else refuses the call (and logs the refusal). A method without a
    /// rule is refused for everyone.
    template <typename F>
    grpc::Status guarded(grpc::ServerContext* context, std::string_view method, F&& body)
    {
        const RpcRule* rule = find_rule(kControlService, method);
        const auto found = auth_.identify(*context);
        if (rule != nullptr && found && allows(*found, rule->need))
        {
            return body(Call{*found, rule, context});
        }
        const std::string what = rule != nullptr ? std::string(rule->what) : "call " + std::string(method);
        audit_.refused((found ? found->name : std::string("unauthenticated")) + "@" + context->peer() + " " + what + ": refused");
        if (!found)
        {
            return grpc::Status(grpc::StatusCode::UNAUTHENTICATED, "a valid token is required");
        }
        if (rule == nullptr)
        {
            return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                "the server has no authorization rule for '" + std::string(method) + "', so nobody may call it");
        }
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED, "the role " + role_name(found->role) + " may not " + what);
    }

    /// Logs the outcome of an allowed call, if its rule says it is audited.
    void audit(const Call& call, const std::string& what, const std::string& outcome)
    {
        if (call.rule->audited)
        {
            audit_(call.who.name + "@" + call.context->peer() + " " + what + ": " + outcome);
        }
    }

    grpc::Status Ping(grpc::ServerContext* context, const v1::PingRequest* request, v1::PingReply* reply) override
    {
        return guarded(context, "Ping", [&](const Call&) -> grpc::Status {
            reply->set_text(request->text());
            reply->set_server_version("vehicle-sil control v1");
            return grpc::Status::OK;
        });
    }

    grpc::Status ListPeers(grpc::ServerContext* context, const v1::ListPeersRequest*, v1::ListPeersReply* reply) override
    {
        return guarded(context, "ListPeers", [&](const Call&) -> grpc::Status {
            // The agents that connected, joined with the participants SIL Kit knows (with a time
            // control: the simulation's participants on every PC, also those without an agent).
            std::optional<SimulationSnapshot> snapshot;
            if (monitor_ != nullptr)
            {
                snapshot = monitor_->snapshot();
                if (!snapshot->participants_error.empty())
                {
                    reply->set_simulation_error("cannot read the participants' states: " + snapshot->participants_error);
                }
            }
            const auto agents = hub_.peers();
            for (const auto& p : agents)
            {
                detail::peer_to_proto(p.participant, &p, snapshot ? &*snapshot : nullptr, *reply->add_peers());
            }
            if (snapshot)
            {
                for (const auto& [name, state] : snapshot->participants)
                {
                    const auto known = std::any_of(agents.begin(), agents.end(), [&](const PeerInfo& p) { return p.participant == name; });
                    if (!known)
                    {
                        detail::peer_to_proto(name, nullptr, &*snapshot, *reply->add_peers());
                    }
                }
            }
            return grpc::Status::OK;
        });
    }

    grpc::Status GetValues(grpc::ServerContext* context, const v1::GetValuesRequest* request, v1::GetValuesReply* reply) override
    {
        return guarded(context, "GetValues", [&](const Call&) -> grpc::Status {
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
        });
    }

    grpc::Status Write(grpc::ServerContext* context, const v1::WriteRequest* request, v1::WriteReply* reply) override
    {
        return guarded(context, "Write", [&](const Call& call) -> grpc::Status {
            if (static_cast<std::size_t>(request->writes_size()) > options_.max_writes_per_request)
            {
                audit(call, "write " + std::to_string(request->writes_size()) + " values", "refused (too many)");
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
            r.client = call.who.name + "@" + peer_of(*context);
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
                audit(call, what, "seq " + std::to_string(seq));
            }
            catch (const std::exception& e)
            {
                audit(call, what, e.what());
                return error_status(e);
            }
            return grpc::Status::OK;
        });
    }

    grpc::Status GetOperation(grpc::ServerContext* context, const v1::GetOperationRequest* request, v1::Operation* reply) override
    {
        return guarded(context, "GetOperation", [&](const Call&) -> grpc::Status {
            const auto op = hub_.operation(request->seq());
            if (!op)
            {
                return grpc::Status(grpc::StatusCode::NOT_FOUND, "no operation " + std::to_string(request->seq()));
            }
            detail::to_proto(*op, *reply);
            return grpc::Status::OK;
        });
    }

    grpc::Status WaitOperation(grpc::ServerContext* context, const v1::WaitOperationRequest* request, v1::Operation* reply) override
    {
        return guarded(context, "WaitOperation", [&](const Call&) -> grpc::Status {
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
        });
    }

    grpc::Status Watch(grpc::ServerContext* context, const v1::WatchRequest* request, grpc::ServerWriter<v1::Update>* writer) override
    {
        return guarded(context, "Watch", [&](const Call&) -> grpc::Status {
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
            const bool watch_time = request->time() && monitor_ != nullptr;
            const bool watch_simulation = request->simulation();
            const auto wants = [&o](const std::string& peer) {
                return o.peers.empty() || std::find(o.peers.begin(), o.peers.end(), peer) != o.peers.end();
            };
            const auto sub = hub_.subscribe(o);
            const auto write = [&](v1::Update& out) {
                out.set_dropped(sub->dropped());
                return writer->Write(out);
            };
            // The monitor's changes, in order, taken by this stream between its waits on the hub.
            struct Pending
            {
                std::mutex mutex;
                std::deque<SimulationSnapshot> snapshots;
            } pending;
            SimulationMonitor::Subscription simulation_sub;
            std::optional<SimulationSnapshot> current; ///< taken from the monitor, not handled yet
            std::optional<SimulationSnapshot> known;   ///< the latest handled: the peers' SIL Kit states
            if (monitor_ != nullptr)
            {
                simulation_sub = monitor_->subscribe([&pending](const SimulationSnapshot& snapshot) {
                    const std::lock_guard lock(pending.mutex);
                    if (pending.snapshots.size() >= kMaxPendingSnapshots)
                    {
                        pending.snapshots.pop_front(); // a slow client misses intermediate states
                    }
                    pending.snapshots.push_back(snapshot);
                });
                current = monitor_->snapshot();
                known = current;
            }
            // The peers as ListPeers reports them (the hub's, then the participants without an agent),
            // kept up to date: the hub's peers of this watch and the state each was last sent with.
            std::map<std::string, PeerInfo> agents;
            std::map<std::string, std::string> sent_states;
            const auto send_peer = [&](const std::string& name) {
                const auto agent = agents.find(name);
                const bool participant = known && std::any_of(known->participants.begin(), known->participants.end(),
                                                               [&](const auto& p) { return p.first == name; });
                v1::Update out;
                if (agent == agents.end() && !participant)
                {
                    sent_states.erase(name);
                    out.set_removed_peer(name);
                    return write(out);
                }
                detail::peer_to_proto(name, agent != agents.end() ? &agent->second : nullptr, known ? &*known : nullptr, *out.mutable_peer());
                sent_states[name] = out.peer().simulation_state();
                return write(out);
            };
            std::optional<TimeView> sent_time;
            std::optional<SimulationSnapshot> sent_simulation;
            bool sent_unavailable = false;
            // Sends what changed in the simulation since the last time; false if the client went away.
            const auto send_simulation = [&]() {
                if (watch_simulation && monitor_ == nullptr && !std::exchange(sent_unavailable, true))
                {
                    v1::Update out;
                    detail::to_proto(nullptr, *out.mutable_simulation());
                    return write(out);
                }
                if (!current)
                {
                    return true;
                }
                known = std::move(current);
                current.reset();
                const auto& snapshot = *known;
                if (watch_time && snapshot.time_error.empty() && sent_time != snapshot.time)
                {
                    v1::Update out;
                    detail::time_to_proto(snapshot.time, *out.mutable_time());
                    if (!write(out))
                    {
                        return false;
                    }
                    sent_time = snapshot.time;
                }
                if (watch_simulation && (!sent_simulation || !sent_simulation->same_state(snapshot)))
                {
                    v1::Update out;
                    detail::to_proto(&snapshot, *out.mutable_simulation());
                    if (!write(out))
                    {
                        return false;
                    }
                    sent_simulation = snapshot;
                }
                // The peers whose SIL Kit state changed, and participants not sent yet.
                for (const auto& [name, state] : snapshot.participants)
                {
                    const auto sent = sent_states.find(name);
                    if (wants(name) && (sent == sent_states.end() || sent->second != state) && !send_peer(name))
                    {
                        return false;
                    }
                }
                return true;
            };
            bool initial = true;
            while (!context->IsCancelled() && !stopping_.load())
            {
                // The first poll takes everything the subscription starts with (peers, latest values).
                for (auto& update : sub->poll(initial ? 0 : 64, initial ? std::chrono::milliseconds(0) : std::chrono::milliseconds(100)))
                {
                    bool sent = true;
                    if (const auto* p = std::get_if<PeerUpdate>(&update))
                    {
                        if (p->removed)
                        {
                            agents.erase(p->peer.participant);
                        }
                        else
                        {
                            agents.insert_or_assign(p->peer.participant, p->peer);
                        }
                        sent = send_peer(p->peer.participant);
                    }
                    else
                    {
                        v1::Update out;
                        if (const auto* v = std::get_if<ValuesUpdate>(&update))
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
                        sent = write(out);
                    }
                    if (!sent)
                    {
                        sub->close();
                        return grpc::Status::OK; // the client went away
                    }
                }
                std::deque<SimulationSnapshot> taken;
                {
                    const std::lock_guard lock(pending.mutex);
                    taken.swap(pending.snapshots);
                }
                bool sent = send_simulation(); // the initial snapshot, or the unavailable one
                for (auto& snapshot : taken)
                {
                    if (!sent)
                    {
                        break;
                    }
                    if (!known || snapshot.version > known->version)
                    {
                        current = std::move(snapshot);
                        sent = send_simulation();
                    }
                }
                if (!sent)
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
        });
    }

    grpc::Status ForgetPeer(grpc::ServerContext* context, const v1::ForgetPeerRequest* request, v1::ForgetPeerReply*) override
    {
        return guarded(context, "ForgetPeer", [&](const Call& call) -> grpc::Status {
            const bool forgotten = hub_.forget_peer(request->peer());
            audit(call, "forget " + request->peer(), forgotten ? "ok" : "refused (unknown or connected)");
            if (!forgotten)
            {
                return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, "no disconnected peer " + request->peer());
            }
            return grpc::Status::OK;
        });
    }

    // ---- time ----------------------------------------------------------------------------

    template <typename F>
    grpc::Status with_time(v1::TimeState* reply, F&& f, const Call* call = nullptr, const char* what = nullptr)
    {
        auto status = with_time_impl(reply, std::forward<F>(f));
        if (call != nullptr)
        {
            audit(*call, what != nullptr ? what : std::string(call->rule->what), status.ok() ? "ok" : status.error_message());
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
            // The watches see the change at once, not only at the monitor's next read.
            monitor_->poll();
            detail::time_to_proto(options_.time->state(), *reply);
        }
        catch (const std::exception& e)
        {
            return error_status(e);
        }
        return grpc::Status::OK;
    }

    grpc::Status GetTime(grpc::ServerContext* context, const v1::GetTimeRequest*, v1::TimeState* reply) override
    {
        return guarded(context, "GetTime", [&](const Call&) -> grpc::Status {
            if (options_.time == nullptr)
            {
                reply->set_available(false);
                return grpc::Status::OK;
            }
            return with_time(reply, [](TimeControl&) {});
        });
    }
    grpc::Status Pause(grpc::ServerContext* context, const v1::PauseRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "Pause", [&](const Call& call) -> grpc::Status {
            return with_time(reply, [&](TimeControl& t) { t.pause(r->has_at_ns() ? std::optional(time_of(r->at_ns())) : std::nullopt); }, &call);
        });
    }
    grpc::Status Resume(grpc::ServerContext* context, const v1::ResumeRequest*, v1::TimeState* reply) override
    {
        return guarded(context, "Resume", [&](const Call& call) -> grpc::Status {
            return with_time(reply, [](TimeControl& t) { t.resume(); }, &call);
        });
    }
    grpc::Status RunUntil(grpc::ServerContext* context, const v1::RunUntilRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "RunUntil", [&](const Call& call) -> grpc::Status {
            return with_time(reply, [&](TimeControl& t) { t.run_until(time_of(r->at_ns())); }, &call);
        });
    }
    grpc::Status Step(grpc::ServerContext* context, const v1::StepRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "Step", [&](const Call& call) -> grpc::Status {
            return with_time(reply, [&](TimeControl& t) { t.step(std::chrono::nanoseconds(r->duration_ns())); }, &call);
        });
    }
    grpc::Status SetSpeed(grpc::ServerContext* context, const v1::SetSpeedRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "SetSpeed", [&](const Call& call) -> grpc::Status {
            return with_time(reply, [&](TimeControl& t) { t.set_speed(r->has_speed() ? std::optional(r->speed()) : std::nullopt); }, &call);
        });
    }
    grpc::Status WaitPaused(grpc::ServerContext* context, const v1::WaitPausedRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "WaitPaused", [&](const Call&) -> grpc::Status {
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
        });
    }
    grpc::Status Stop(grpc::ServerContext* context, const v1::StopRequest* r, v1::TimeState* reply) override
    {
        return guarded(context, "Stop", [&](const Call& call) -> grpc::Status {
            // The reason goes to every participant and into SIL Kit's logs: cleaned and bounded.
            constexpr std::size_t kMaxReason = 200;
            auto reason = clean(cut_utf8(r->reason(), kMaxReason));
            if (reason.empty())
            {
                reason = "stopped by a client";
            }
            const auto what = "stop (" + reason + ")";
            return with_time(reply, [&](TimeControl& t) { t.stop(reason); }, &call, what.c_str());
        });
    }

private:
    Hub& hub_;
    const ServerOptions& options_;
    SimulationMonitor* monitor_;
    const std::atomic<bool>& stopping_;
    const Authorizer& auth_;
    const AuditLog& audit_;
    std::atomic<std::size_t> waiting_{0};
};

} // namespace

std::unique_ptr<grpc::Service> make_control_service(Hub& hub, const ServerOptions& options, SimulationMonitor* monitor,
                                                    const std::atomic<bool>& stopping, const Authorizer& auth, const AuditLog& audit)
{
    return std::make_unique<ControlServiceImpl>(hub, options, monitor, stopping, auth, audit);
}

} // namespace vehicle_sil::control_grpc::detail
