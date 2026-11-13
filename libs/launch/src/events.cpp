// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/events.hpp"

#include "check_format.hpp"
#include "yaml_tree.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/ecu/detail/json_writer.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace vehicle_sil::launch {

namespace {

using Writer = vehicle_sil::detail::JsonWriter;

/// The members that `fill` writes, as a compact JSON object.
template <class Fill>
std::string object(Fill fill)
{
    Writer w(true);
    w.begin_object();
    fill(w);
    w.end_object();
    return w.take();
}

void optional_text(Writer& w, std::string_view key, const std::string& value)
{
    if (value.empty())
    {
        w.null(key);
    }
    else
    {
        w.string(key, value);
    }
}

} // namespace

std::string format_version_text() { return std::string("vehicle_sil_launch ") + version() + "\n"; }

std::string format_version_json(const std::vector<std::string_view>& more)
{
    Writer w;
    w.begin_object();
    w.integer("vehicle_sil_launch", kVersionFormat);
    w.string("vehicle_sil", version());
    w.begin_object("formats");
    w.integer("check", kCheckFormat);
    w.integer("run", kRunFormat);
    w.integer("events", kEventsFormat);
    w.integer("describe", kEcuDescriptionFormat);
    w.end_object();
    w.begin_array("features");
    for (const auto feature : kFeatures)
    {
        w.string({}, feature);
    }
    for (const auto feature : more)
    {
        w.string({}, feature);
    }
    w.end_array();
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

const char* phase_name(Phase phase)
{
    switch (phase)
    {
    case Phase::Checking: return "checking";
    case Phase::Starting: return "starting";
    case Phase::Waiting: return "waiting";
    case Phase::Running: return "running";
    case Phase::Stopping: return "stopping";
    case Phase::Ended: return "ended";
    }
    return "ended";
}

const char* participant_state_name(ParticipantState state)
{
    switch (state)
    {
    case ParticipantState::Disabled: return "disabled";
    case ParticipantState::NotStarted: return "not_started";
    case ParticipantState::Started: return "started";
    case ParticipantState::Manual: return "manual";
    case ParticipantState::Joined: return "joined";
    case ParticipantState::Failed: return "failed";
    case ParticipantState::Ended: return "ended";
    case ParticipantState::StopRequested: return "stop_requested";
    case ParticipantState::Killed: return "killed";
    }
    return "ended";
}

// --- commands ---------------------------------------------------------------------------------

std::optional<Command> parse_command(std::string_view line)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }
    const auto first = line.find_first_not_of(" \t");
    if (first == std::string_view::npos)
    {
        return std::nullopt;
    }
    Command out;
    const std::string kExpected = "write one JSON object per line, such as {\"command\":\"stop\"}";
    if (line[first] != '{')
    {
        out.message = "the line is not a JSON object: " + kExpected;
        return out;
    }
    try
    {
        const detail::YamlDocument doc(line);
        const ryml::Tree& tree = doc.tree();
        const auto root = doc.root();
        if (root == ryml::NONE || !tree.is_map(root))
        {
            out.message = "the line is not a JSON object: " + kExpected;
            return out;
        }
        const auto node = tree.find_child(root, ryml::csubstr("command", 7));
        if (node == ryml::NONE || !tree.has_val(node) || tree.is_container(node) || !tree.is_val_quoted(node))
        {
            out.message = std::string(node == ryml::NONE ? "the object has no \"command\"" : "\"command\" is not a string") +
                          ": " + kExpected;
            return out;
        }
        out.name = detail::to_string(tree.val(node));
        const auto scalar = [&](const char* key) -> std::optional<std::pair<std::string, bool>> {
            const auto n = tree.find_child(root, ryml::to_csubstr(key));
            if (n == ryml::NONE || !tree.has_val(n) || tree.is_container(n))
            {
                return std::nullopt;
            }
            return std::pair{detail::to_string(tree.val(n)), tree.is_val_quoted(n)};
        };
        if (*out.name == "step")
        {
            const auto ms = scalar("ms");
            std::int64_t value = 0;
            const char* end = ms ? ms->first.data() + ms->first.size() : nullptr;
            if (!ms || ms->second || ms->first.empty() || std::from_chars(ms->first.data(), end, value).ptr != end ||
                value < 1 || value > kMaxMillis)
            {
                out.message = "step needs \"ms\", a whole number of milliseconds of virtual time from 1 to " +
                              std::to_string(kMaxMillis) + ", such as {\"command\":\"step\",\"ms\":100}";
                return out;
            }
            out.ms = value;
        }
        else if (*out.name == "speed")
        {
            const auto value = scalar("value");
            if (!value || value->second != (value->first == "max") || !parse_speed(value->first, out.speed))
            {
                out.message = "speed needs \"value\", a number of simulation seconds per wall-clock second from 0.000001 "
                              "to 1000000 or \"max\", such as {\"command\":\"speed\",\"value\":1}";
                return out;
            }
        }
    }
    catch (const detail::YamlSyntaxError&)
    {
        out.message = "the line is not valid JSON: " + kExpected;
        return out;
    }
    static constexpr std::string_view kNames[] = {"stop", "pause", "resume", "step", "speed"};
    if (std::find(std::begin(kNames), std::end(kNames), *out.name) == std::end(kNames))
    {
        // The name as far as it is worth showing.
        const std::string shown = out.name->size() > 64 ? out.name->substr(0, 64) + "..." : *out.name;
        out.message = "unknown command '" + shown + "': the commands are stop, pause, resume, step and speed";
        return out;
    }
    out.accepted = true;
    return out;
}

void CommandInput::append(std::string_view bytes)
{
    while (!bytes.empty())
    {
        const auto end = bytes.find('\n');
        const std::string_view part = bytes.substr(0, end);
        if (!skipping_)
        {
            pending_.append(part);
            if (pending_.size() > kMaxCommandLine)
            {
                Command tooLong;
                tooLong.message = "the line is longer than " + std::to_string(kMaxCommandLine) + " bytes";
                on_command_(tooLong);
                pending_.clear();
                skipping_ = true;
            }
        }
        if (end == std::string_view::npos)
        {
            return;
        }
        if (!skipping_)
        {
            line(pending_);
        }
        pending_.clear();
        skipping_ = false;
        bytes.remove_prefix(end + 1);
    }
}

void CommandInput::finish()
{
    if (!skipping_ && !pending_.empty())
    {
        line(pending_);
    }
    pending_.clear();
    skipping_ = false;
}

void CommandInput::line(std::string_view text)
{
    if (auto command = parse_command(text))
    {
        on_command_(*command);
    }
}

// --- events -----------------------------------------------------------------------------------

namespace {

using Clock = std::chrono::steady_clock;

/// The most bytes given to `write` at once: a reader that reads is seen at least this often (a
/// write that waits says nothing of the bytes it has passed on), and a pipe takes this much in
/// one piece.
constexpr std::size_t kWriteChunk = 4096;
/// result() asks its caller whether to give up only once nothing could be written for this long:
/// a reader that reads gets the result, whatever the caller wants.
constexpr std::chrono::milliseconds kGiveUpAfter{500};
/// How long the destructor waits for the two threads.
constexpr std::chrono::seconds kThreadsEnd{5};

/// The "log" events dropped at one place of the stream.
struct Dropped
{
    std::int64_t count = 0;
    std::map<std::string, std::int64_t> sources;
};

/// One line that waits to be written.
struct Line
{
    std::string event;
    std::string members; ///< a compact JSON object
    std::int64_t t_ms = 0;
    bool log = false;
    bool last = false;
    /// Set: the place of dropped "log" events ("log_dropped"; `members` is made when it is written).
    std::unique_ptr<Dropped> dropped{};
};

} // namespace

/// What the stream and its two threads share. Everything but the functions and the options is
/// guarded by `mutex`, which is never held across `write`, `on_closed` or a wait for the reader.
struct EventStream::State
{
    Write write;
    OnClosed on_closed;
    EventStreamOptions options;

    std::mutex mutex;
    std::condition_variable work;    ///< for the writer: a line, the end
    std::condition_variable changed; ///< for whoever waits for the writer: a line is written, a thread has ended
    std::condition_variable alarm;   ///< for the watch: closed, the result is written, the end

    std::deque<Line> queue;
    std::size_t log_lines = 0;
    std::size_t log_bytes = 0;
    std::size_t other_events = 0;
    Dropped* dropping = nullptr; ///< the last line of the queue's "log_dropped", while no "log" follows it

    std::int64_t seq = 0;        ///< of the next line written
    bool writing = false;        ///< the writer is in a line
    Clock::time_point progress{}; ///< when the line began or its last byte was taken
    Clock::time_point written{};  ///< when the last line was finished
    bool sealed = false;         ///< the result has been given
    bool finished = false;       ///< the result has been written
    bool closed = false;
    bool told = false;           ///< on_closed has been called, or need not be
    std::string reason;
    bool ending = false;         ///< the destructor runs
    int threads = 0;

    /// The stream closes (the caller holds `mutex`). Nothing after the result has been written.
    void close(std::string why)
    {
        if (closed || finished)
        {
            return;
        }
        closed = true;
        reason = std::move(why);
        told = sealed || !on_closed; // after the result the run is over: result() says it
        queue.clear();
        dropping = nullptr;
        log_lines = log_bytes = other_events = 0;
        work.notify_all();
        changed.notify_all();
        alarm.notify_all();
    }
};

EventStream::EventStream(Write write, OnClosed on_closed, EventStreamOptions options) : state_(std::make_shared<State>())
{
    state_->write = std::move(write);
    state_->on_closed = std::move(on_closed);
    state_->options = std::move(options);
    if (!state_->options.now_ms)
    {
        const auto start = Clock::now();
        state_->options.now_ms = [start] {
            return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        };
    }
    state_->threads = 2;
    try
    {
        std::thread([state = state_] { write_lines(state); }).detach();
    }
    catch (...)
    {
        state_->threads = 0;
        throw;
    }
    try
    {
        std::thread([state = state_] { watch(state); }).detach();
    }
    catch (...)
    {
        const std::lock_guard lock(state_->mutex);
        state_->threads = 1;
        state_->ending = true;
        state_->work.notify_all();
        throw;
    }
}

EventStream::~EventStream()
{
    std::unique_lock lock(state_->mutex);
    state_->ending = true;
    state_->work.notify_all();
    state_->alarm.notify_all();
    // A writer that is still in `write` is left behind (it holds the state).
    state_->changed.wait_for(lock, kThreadsEnd, [this] { return state_->threads == 0; });
}

/// The writer: takes the lines in their order, gives each its "seq" and writes it; writes a
/// heartbeat when there has been no line for a while. Ends with the result, or when the stream
/// closes.
void EventStream::write_lines(const std::shared_ptr<State>& state)
{
    State& s = *state;
    std::unique_lock lock(s.mutex);
    const auto leave = [&s] {
        --s.threads;
        s.changed.notify_all();
    };
    while (true)
    {
        const auto ready = [&s] { return !s.queue.empty() || s.closed || s.ending; };
        // A heartbeat: after the first line, before the result.
        if (s.seq > 0 && !s.sealed && s.options.heartbeat.count() > 0)
        {
            s.work.wait_until(lock, s.written + s.options.heartbeat, ready);
        }
        else
        {
            s.work.wait(lock, ready);
        }
        if (s.closed || s.ending)
        {
            return leave();
        }
        Line line;
        if (s.queue.empty())
        {
            if (s.sealed || Clock::now() < s.written + s.options.heartbeat)
            {
                continue;
            }
            line.event = "heartbeat";
            line.members = "{}";
            line.t_ms = s.options.now_ms();
        }
        else
        {
            line = std::move(s.queue.front());
            s.queue.pop_front();
            if (line.dropped)
            {
                // What was dropped here up to now; what is dropped from now on gets a place of its own.
                if (s.dropping == line.dropped.get())
                {
                    s.dropping = nullptr;
                }
                line.members = object([&line](Writer& w) {
                    w.integer("count", line.dropped->count);
                    w.begin_object("sources");
                    for (const auto& [source, count] : line.dropped->sources)
                    {
                        w.integer(source, count);
                    }
                    w.end_object();
                });
            }
            else if (line.log)
            {
                --s.log_lines;
                s.log_bytes -= line.members.size();
            }
            else
            {
                --s.other_events;
            }
        }
        Writer head(true);
        head.begin_object();
        head.string("event", line.event);
        head.integer("seq", s.seq++);
        head.integer("t_ms", line.t_ms);
        std::string text = head.take(); // "{...": the object is still open
        // `members` is "{}" or "{<members>}": its members after the head's.
        if (line.members.size() > 2)
        {
            text += ',';
            text.append(line.members, 1, std::string::npos);
        }
        else
        {
            text += '}';
        }
        text += '\n';
        s.writing = true;
        s.progress = Clock::now();
        for (std::size_t done = 0; done < text.size();)
        {
            // A piece ends between two characters (a console takes whole ones).
            std::size_t size = std::min(kWriteChunk, text.size() - done);
            while (size > 1 && done + size < text.size() && (static_cast<unsigned char>(text[done + size]) & 0xC0) == 0x80)
            {
                --size;
            }
            lock.unlock();
            // The only wait for the reader of the events.
            const EventWrite wrote = s.write(std::string_view(text).substr(done, size));
            lock.lock();
            if (s.closed || s.ending)
            {
                return leave();
            }
            if (wrote.failed)
            {
                s.close(wrote.error.empty() ? std::string("the write failed") : wrote.error);
                return leave();
            }
            if (wrote.bytes > 0)
            {
                done += std::min(wrote.bytes, text.size() - done);
                s.progress = Clock::now();
            }
        }
        s.writing = false;
        s.written = Clock::now();
        s.changed.notify_all();
        if (line.last)
        {
            s.finished = true;
            s.changed.notify_all();
            s.alarm.notify_all();
            return leave();
        }
    }
}

/// The watch: closes the stream when its reader does not read, and tells the caller that it has
/// closed. Ends with the stream.
void EventStream::watch(const std::shared_ptr<State>& state)
{
    State& s = *state;
    std::unique_lock lock(s.mutex);
    const auto limit = s.options.stall_timeout;
    const auto look = std::clamp(limit / 8, std::chrono::milliseconds(1), std::chrono::milliseconds(200));
    while (!s.ending && !s.finished)
    {
        if (s.closed)
        {
            if (!s.told)
            {
                s.told = true;
                const std::string reason = s.reason;
                lock.unlock();
                s.on_closed(reason);
                lock.lock();
            }
            break;
        }
        const auto now = Clock::now();
        if (s.writing && now - s.progress >= limit)
        {
            s.close("their reader has not read for " + std::to_string(limit.count()) + " ms");
            continue;
        }
        s.alarm.wait_for(lock, look);
    }
    --s.threads;
    s.changed.notify_all();
}

bool EventStream::emit(std::string_view event, std::string members, Kind kind, const std::string& source)
{
    State& s = *state_;
    const std::lock_guard lock(s.mutex);
    if (s.closed || s.sealed || s.ending)
    {
        return false;
    }
    Line line;
    line.t_ms = s.options.now_ms();
    if (kind == Kind::Log)
    {
        if (s.log_lines > 0 && (s.log_lines >= s.options.max_log_lines || s.log_bytes + members.size() > s.options.max_log_bytes))
        {
            // No room: counted at this place of the stream instead.
            if (s.dropping == nullptr)
            {
                Line place;
                place.event = "log_dropped";
                place.t_ms = line.t_ms;
                place.dropped = std::make_unique<Dropped>();
                s.dropping = place.dropped.get();
                s.queue.push_back(std::move(place));
                s.work.notify_one();
            }
            ++s.dropping->count;
            ++s.dropping->sources[source];
            return false;
        }
        ++s.log_lines;
        s.log_bytes += members.size();
        s.dropping = nullptr;
        line.log = true;
    }
    else if (kind == Kind::Other)
    {
        if (s.other_events >= s.options.max_other_events)
        {
            s.close("more than " + std::to_string(s.options.max_other_events) + " events wait to be written");
            return false;
        }
        ++s.other_events;
    }
    else
    {
        ++s.other_events;
        s.sealed = true;
        line.last = true;
    }
    line.event = std::string(event);
    line.members = std::move(members);
    s.queue.push_back(std::move(line));
    s.work.notify_one();
    return true;
}

bool EventStream::wait_written(std::chrono::milliseconds timeout)
{
    State& s = *state_;
    std::unique_lock lock(s.mutex);
    return s.changed.wait_for(lock, timeout, [&s] { return s.closed || (s.queue.empty() && !s.writing); }) && !s.closed;
}

std::size_t EventStream::waiting_events() const
{
    const std::lock_guard lock(state_->mutex);
    return state_->other_events;
}

bool EventStream::closed() const
{
    const std::lock_guard lock(state_->mutex);
    return state_->closed;
}

std::string EventStream::close_reason() const
{
    const std::lock_guard lock(state_->mutex);
    return state_->reason;
}

void EventStream::hello(long pid, const std::string& file, const std::vector<std::string>& arguments)
{
    emit("hello", object([&](Writer& w) {
             w.integer("vehicle_sil_events", kEventsFormat);
             w.string("vehicle_sil", version());
             w.integer("pid", pid);
             optional_text(w, "file", file);
             w.begin_array("arguments");
             for (const auto& argument : arguments)
             {
                 w.string({}, argument);
             }
             w.end_array();
         }));
}

void EventStream::phase(Phase phase)
{
    emit("phase", object([&](Writer& w) { w.string("phase", phase_name(phase)); }));
}

void EventStream::check(const CheckReport& report)
{
    emit("check", object([&](Writer& w) { detail::write_check_json(w, "check", report); }));
}

void EventStream::registry(const std::string& uri)
{
    emit("registry", object([&](Writer& w) { w.string("uri", uri); }));
}

void EventStream::control_server(const std::string& address, bool tls, bool credentials)
{
    emit("control_server", object([&](Writer& w) {
             w.string("address", address);
             w.boolean("tls", tls);
             w.boolean("credentials", credentials);
         }));
}

void EventStream::participant(const std::string& name, ParticipantState state)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(state));
         }));
}

void EventStream::control_link(const std::string& name, std::string_view state, std::optional<std::size_t> variables,
                               std::optional<int> version)
{
    emit("control_link", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", state);
             if (variables)
             {
                 w.integer("variables", static_cast<std::int64_t>(*variables));
             }
             if (version)
             {
                 w.integer("version", *version);
             }
         }));
}

void EventStream::scenario_step(const ScenarioStepRun& step)
{
    emit("scenario_step", object([&](Writer& w) { detail::write_scenario_step_members(w, step); }));
}

void EventStream::participant_not_started(const std::string& name, const std::string& message)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(ParticipantState::NotStarted));
             w.string("message", message);
         }));
}

void EventStream::participant_failed(const std::string& name, std::string_view reason, const std::string& message)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(ParticipantState::Failed));
             w.string("reason", reason);
             if (!message.empty())
             {
                 w.string("message", message);
             }
         }));
}

void EventStream::participant_started(const std::string& name, long pid, const std::vector<std::string>& command,
                                      const std::string& log)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(ParticipantState::Started));
             w.integer("pid", pid);
             w.begin_array("command");
             for (const auto& word : command)
             {
                 w.string({}, word);
             }
             w.end_array();
             optional_text(w, "log", log);
         }));
}

void EventStream::participant_manual(const std::string& name, const std::vector<std::string>& command,
                                     const std::string& working_directory, const std::string& command_line,
                                     const std::vector<std::pair<std::string, std::string>>& environment)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(ParticipantState::Manual));
             w.begin_array("command");
             for (const auto& word : command)
             {
                 w.string({}, word);
             }
             w.end_array();
             w.string("working_directory", working_directory);
             w.string("command_line", command_line);
             if (!environment.empty())
             {
                 w.begin_object("environment");
                 for (const auto& [variable, value] : environment)
                 {
                     w.string(variable, value);
                 }
                 w.end_object();
             }
         }));
}

void EventStream::participant_ended(const std::string& name, const process::ExitStatus& status)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(ParticipantState::Ended));
             // As the result's participants[]: the exit code, or the signal.
             if (status.signal)
             {
                 w.null("exit_code");
                 w.string("signal", process::signal_name(*status.signal));
             }
             else
             {
                 w.integer("exit_code", status.code);
                 w.null("signal");
             }
         }));
}

void EventStream::log(const std::string& source, const std::string& line)
{
    emit("log", object([&](Writer& w) {
             w.string("source", source);
             w.string("line", line);
         }),
         Kind::Log, source);
}

void EventStream::finding(const std::string& file, const Finding& finding)
{
    emit("finding", object([&](Writer& w) { detail::write_finding_members(w, file, finding); }));
}

void EventStream::time(const TimeEvent& time)
{
    emit("time", object([&](Writer& w) {
             w.integer("sim_ms", time.sim_ms);
             if (time.speed)
             {
                 w.real("speed", time.speed);
             }
             else
             {
                 w.string("speed", "max");
             }
             w.boolean("paused", time.paused);
             w.real("actual_speed", time.actual_speed);
             w.integer("late_steps", static_cast<std::int64_t>(time.late_steps));
             w.integer("max_late_ms", time.max_late_ms);
             w.integer("slipped_ms", time.slipped_ms);
             w.integer("late_ms", time.late_ms);
         }));
}

void EventStream::command(const Command& command, std::string_view source)
{
    emit("command", object([&](Writer& w) {
             if (command.name)
             {
                 w.string("command", *command.name);
             }
             else
             {
                 w.null("command");
             }
             w.string("source", source);
             w.boolean("accepted", command.accepted);
             optional_text(w, "message", command.message);
             // step: how far the simulation goes (a whole number of the controller's steps).
             if (command.accepted && command.name == "step" && command.ms)
             {
                 w.integer("ms", *command.ms);
             }
             // pause at, run_until (a control client): where it will stand.
             if (command.accepted && command.at_ms)
             {
                 w.integer("at_ms", *command.at_ms);
             }
         }));
}

bool EventStream::result(const RunReport& report, const std::function<bool()>& give_up)
{
    if (!emit("result", object([&](Writer& w) { detail::write_run_json(w, "result", report); }), Kind::Result))
    {
        return false;
    }
    State& s = *state_;
    std::unique_lock lock(s.mutex);
    // The limit of this wait is its own: a reader that reads, but too slowly, is no reader that
    // does not read (the watch's limit).
    const auto start = Clock::now();
    const auto deadline = start + s.options.stall_timeout;
    while (!s.finished && !s.closed)
    {
        const auto now = Clock::now();
        if (now >= deadline)
        {
            s.close("the result could not be written within " + std::to_string(s.options.stall_timeout.count()) + " ms");
            break;
        }
        if (give_up && now - std::max({start, s.progress, s.written}) >= kGiveUpAfter)
        {
            lock.unlock();
            const bool over = give_up();
            lock.lock();
            if (over && !s.finished)
            {
                s.close("the result was not read, and the wait for its reader was stopped");
                break;
            }
        }
        s.changed.wait_for(lock, std::chrono::milliseconds(10));
    }
    return s.finished;
}

} // namespace vehicle_sil::launch
