// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/events.hpp"

#include "check_format.hpp"
#include "run_format.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/json/json_writer.hpp"
#include "vehicle_sil/launch/connections.hpp"
#include "vehicle_sil/launch/control_metrics_format.hpp"
#include "vehicle_sil/launch/execution_runtime.hpp"
#include "vehicle_sil/launch/test.hpp"
#include "vehicle_sil/launch/timeline_format.hpp"

#include <algorithm>
#include <iterator>
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

using Writer = vehicle_sil::json::JsonWriter;

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

/// `members` (a compact JSON object) with "run":`run` as its first member (a run's window).
std::string with_run(const std::string& run, const std::string& members)
{
    Writer head(true);
    head.begin_object();
    head.string("run", run);
    std::string out = head.take(); // "{"run":...": the object is still open
    if (members.size() > 2)
    {
        out += ',';
        out.append(members, 1, std::string::npos);
    }
    else
    {
        out += '}';
    }
    return out;
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
    w.integer("test", kTestFormat);
    w.integer("timeline", kTimelineFormat);
    w.integer("connections", kConnectionsFormat);
    w.integer("control_metrics", kControlMetricsFormat);
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

const char* participant_state_name(runner::ParticipantState state)
{
    switch (state)
    {
    case runner::ParticipantState::Disabled: return "disabled";
    case runner::ParticipantState::NotStarted: return "not_started";
    case runner::ParticipantState::Started: return "started";
    case runner::ParticipantState::Manual: return "manual";
    case runner::ParticipantState::Joined: return "joined";
    case runner::ParticipantState::Failed: return "failed";
    case runner::ParticipantState::Ended: return "ended";
    case runner::ParticipantState::StopRequested: return "stop_requested";
    case runner::ParticipantState::Killed: return "killed";
    }
    return "ended";
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
    bool state = false; ///< a "state": at most one waits, and it is counted nowhere
    bool last = false;
    /// Set: the place of dropped "log" events ("log_dropped"; `members` is made when it is written).
    std::unique_ptr<Dropped> dropped{};
    /// The window that made it (serve: EventStream::open_run()); 0: the stream itself.
    std::uint64_t window = 0;
    /// "log_dropped" of a window: its run, put first when `members` is made.
    std::string run{};
    /// Its place in the queue (1, 2, ...), which result() waits for; 0: a heartbeat.
    std::uint64_t order = 0;
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

    std::uint64_t dropping_window = 0; ///< the window of `dropping`

    std::uint64_t queued = 0;      ///< the order of the last line queued
    std::uint64_t done_order = 0;  ///< the order of the last line written
    std::uint64_t next_window = 1; ///< the number of the next window (open_run())
    std::uint64_t open_window = 0; ///< the window open now, whose run the heartbeats name; 0: none
    std::string open_run{};        ///< its run

    std::int64_t seq = 0;        ///< of the next line written
    bool writing = false;        ///< the writer is in a line
    Clock::time_point progress{}; ///< when the line began or its last byte was taken
    Clock::time_point written{};  ///< when the last line was finished
    bool sealed = false;         ///< the result has been given
    bool finished = false;       ///< the result has been written
    bool closed = false;
    bool told = false;           ///< on_closed has been called, or need not be
    bool on_closed_done = false; ///< on_closed has returned, or need not be called
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
        on_closed_done = told;
        queue.clear();
        dropping = nullptr;
        log_lines = log_bytes = other_events = 0;
        work.notify_all();
        changed.notify_all();
        alarm.notify_all();
    }
};

/// A run's window: what it adds to the stream's state (guarded by the state's mutex).
struct EventStream::Window
{
    std::string run;
    std::uint64_t number = 0;
    bool closed = false; ///< its result has been given, or it has been destroyed
};

EventStream::EventStream(std::shared_ptr<State> state, std::shared_ptr<Window> window) :
  state_(std::move(state)), window_(std::move(window))
{
}

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
    if (window_)
    {
        // A window only closes: the stream and its threads are not its.
        if (!window_->closed)
        {
            window_->closed = true;
            if (state_->open_window == window_->number)
            {
                state_->open_window = 0;
                state_->open_run.clear();
            }
        }
        return;
    }
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
            // In a run's window, a heartbeat is the run's line too.
            line.members = s.open_run.empty() ? std::string("{}") : with_run(s.open_run, "{}");
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
                if (!line.run.empty())
                {
                    line.members = with_run(line.run, line.members);
                }
            }
            else if (line.log)
            {
                --s.log_lines;
                s.log_bytes -= line.members.size();
            }
            else if (!line.state)
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
        if (line.order != 0)
        {
            s.done_order = line.order;
        }
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
                s.on_closed_done = true;
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

bool EventStream::emit(std::string_view event, std::string members, Kind kind, const std::string& source,
                       std::uint64_t* order)
{
    State& s = *state_;
    if (window_)
    {
        members = with_run(window_->run, members); // the run ID never changes: no lock needed
    }
    const std::lock_guard lock(s.mutex);
    if (s.closed || s.sealed || s.ending || (window_ && window_->closed))
    {
        return false;
    }
    const std::uint64_t window = window_ ? window_->number : 0;
    Line line;
    line.t_ms = s.options.now_ms();
    line.window = window;
    if (kind == Kind::Log)
    {
        if (s.log_lines > 0 && (s.log_lines >= s.options.max_log_lines || s.log_bytes + members.size() > s.options.max_log_bytes))
        {
            // No room: counted at this place of the stream instead (a place of this window's).
            if (s.dropping == nullptr || s.dropping_window != window)
            {
                Line place;
                place.event = "log_dropped";
                place.t_ms = line.t_ms;
                place.dropped = std::make_unique<Dropped>();
                place.window = window;
                place.run = window_ ? window_->run : std::string{};
                place.order = ++s.queued;
                s.dropping = place.dropped.get();
                s.dropping_window = window;
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
    else if (kind == Kind::State)
    {
        // Only the latest state means anything: one that still waits is taken out, and this one
        // goes to the end of the queue, after the history it reflects. So at most one waits, and
        // it is not counted among the events that close the stream.
        const auto old =
          std::find_if(s.queue.rbegin(), s.queue.rend(), [window](const Line& l) { return l.state && l.window == window; });
        if (old != s.queue.rend())
        {
            s.queue.erase(std::next(old).base());
        }
        line.state = true;
    }
    else if (kind == Kind::Other || kind == Kind::Open)
    {
        if (s.other_events >= s.options.max_other_events)
        {
            s.close("more than " + std::to_string(s.options.max_other_events) + " events wait to be written");
            return false;
        }
        ++s.other_events;
        if (kind == Kind::Open && window_)
        {
            s.open_window = window;
            s.open_run = window_->run;
        }
    }
    else if (window_)
    {
        // A run's result: the window's last line; with Kind::ResultFinal (launcher_ends: true),
        // the stream ends here too so nothing follows it.
        ++s.other_events;
        window_->closed = true;
        if (s.open_window == window)
        {
            s.open_window = 0;
            s.open_run.clear();
        }
        if (kind == Kind::ResultFinal)
        {
            s.sealed = true;
            line.last = true;
        }
    }
    else
    {
        ++s.other_events;
        s.sealed = true;
        line.last = true;
    }
    line.event = std::string(event);
    line.members = std::move(members);
    line.order = ++s.queued;
    if (order != nullptr)
    {
        *order = line.order;
    }
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

bool EventStream::wait_on_closed(std::chrono::milliseconds timeout)
{
    State& s = *state_;
    std::unique_lock lock(s.mutex);
    if (!s.closed)
    {
        return true;
    }
    return s.changed.wait_for(lock, timeout, [&s] { return s.on_closed_done || s.ending; }) && s.on_closed_done;
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

namespace {

/// The members of "hello".
std::string hello_members(long pid, const std::string& file, const std::vector<std::string>& arguments,
                          const std::string& launcher)
{
    return object([&](Writer& w) {
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
        w.string("launcher", launcher);
    });
}

/// The members of "command" (EventStream::command()), into an object that is open.
void write_command_members(Writer& w, const runner::Command& command, std::string_view source)
{
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
    // Not accepted: the code of why, for a program.
    optional_text(w, "reason", command.accepted ? std::string{} : command.reason);
    // step: how far the simulation goes (a whole number of the controller's steps).
    if (command.accepted && command.name == "step" && command.ms)
    {
        w.integer("ms", *command.ms);
    }
    // step, run_until, pause at (a control client): where it will stand.
    if (command.accepted && command.at_ms)
    {
        w.integer("at_ms", *command.at_ms);
    }
    // The sender's number, as it came.
    if (command.id)
    {
        w.integer("id", *command.id);
    }
    else
    {
        w.null("id");
    }
}

} // namespace

void EventStream::hello(long pid, const std::string& file, const std::vector<std::string>& arguments,
                        const std::string& launcher)
{
    emit("hello", hello_members(pid, file, arguments, launcher));
}

void EventStream::refused(const runner::Command& command, std::string_view source, const std::optional<std::string>& asked_run)
{
    emit("command", object([&](Writer& w) {
             write_command_members(w, command, source);
             if (asked_run)
             {
                 w.string("asked_run", *asked_run);
             }
             else
             {
                 w.null("asked_run");
             }
         }));
}

std::unique_ptr<EventStream> EventStream::open_run(const std::string& run, std::optional<std::int64_t> id)
{
    auto window = std::make_shared<Window>();
    window->run = run;
    {
        const std::lock_guard lock(state_->mutex);
        window->number = state_->next_window++;
    }
    std::unique_ptr<EventStream> out(new EventStream(state_, std::move(window)));
    out->emit("run_start", object([&](Writer& w) {
                  if (id)
                  {
                      w.integer("id", *id);
                  }
                  else
                  {
                      w.null("id");
                  }
              }),
              Kind::Open);
    return out;
}

void EventStream::phase(runner::Phase phase)
{
    emit("phase", object([&](Writer& w) { w.string("phase", runner::phase_name(phase)); }));
}

void EventStream::check(const Checked& report)
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

void EventStream::participant(const std::string& name, runner::ParticipantState state)
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

void EventStream::scenario_step(const scenario::ScenarioStepRun& step)
{
    emit("scenario_step", object([&](Writer& w) { detail::write_scenario_step_members(w, step); }));
}

void EventStream::participant_not_started(const std::string& name, const std::string& message)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(runner::ParticipantState::NotStarted));
             w.string("message", message);
         }));
}

void EventStream::participant_failed(const std::string& name, std::string_view reason, const std::string& message)
{
    emit("participant", object([&](Writer& w) {
             w.string("name", name);
             w.string("state", participant_state_name(runner::ParticipantState::Failed));
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
             w.string("state", participant_state_name(runner::ParticipantState::Started));
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
             w.string("state", participant_state_name(runner::ParticipantState::Manual));
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
             w.string("state", participant_state_name(runner::ParticipantState::Ended));
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

void EventStream::outside(const std::string& network, std::uint64_t to_outside, std::uint64_t from_outside)
{
    emit("outside", object([&](Writer& w) {
             w.string("network", network);
             w.integer("to_outside", static_cast<std::int64_t>(to_outside));
             w.integer("from_outside", static_cast<std::int64_t>(from_outside));
         }));
}

void EventStream::finding(const std::string& file, const config::Finding& finding)
{
    emit("finding", object([&](Writer& w) { detail::write_finding_members(w, file, finding); }));
}

void EventStream::time(const runner::TimeEvent& time)
{
    emit("time", object([&](Writer& w) {
             w.integer("sim_ms", time.sim_ms);
             w.real("actual_speed", time.actual_speed);
             w.integer("late_steps", static_cast<std::int64_t>(time.late_steps));
             w.integer("max_late_ms", time.max_late_ms);
             w.integer("slipped_ms", time.slipped_ms);
             w.integer("late_ms", time.late_ms);
         }));
}

void EventStream::command(const runner::Command& command, std::string_view source)
{
    emit("command", object([&](Writer& w) { write_command_members(w, command, source); }));
}

namespace {

/// allowed.<key>: whether the operation is taken, why not, and the range of its argument
/// (`min`, `max`: the names of its two keys, null for an operation without one; `whole`: the
/// range is of whole numbers).
void write_allowed(Writer& w, std::string_view key, const runner::Allowed& a, const char* min = nullptr,
                   const char* max = nullptr, bool whole = false)
{
    w.begin_object(key);
    w.boolean("ok", a.ok);
    optional_text(w, "reason", a.reason);
    optional_text(w, "message", a.message);
    if (a.caution)
    {
        w.begin_object("caution");
        w.string("code", a.caution->code);
        w.string("message", a.caution->message);
        w.end_object();
    }
    else
    {
        w.null("caution");
    }
    if (a.range && min != nullptr && whole)
    {
        w.integer(min, static_cast<std::int64_t>(a.range->min));
        w.integer(max, static_cast<std::int64_t>(a.range->max));
    }
    else if (a.range && min != nullptr)
    {
        w.real(min, a.range->min);
        w.real(max, a.range->max);
    }
    w.end_object();
}

void write_run_state_members(Writer& w, const runner::RunState& state)
{
    w.integer("rev", static_cast<std::int64_t>(state.rev));
    w.string("phase", runner::phase_name(state.phase));
    w.boolean("paused", state.paused);
    if (state.paused_at_ms)
    {
        w.integer("paused_at_ms", *state.paused_at_ms);
    }
    else
    {
        w.null("paused_at_ms");
    }
    w.boolean("holding", state.holding);
    if (!state.speed)
    {
        w.null("speed");
    }
    else if (*state.speed)
    {
        w.real("speed", *state.speed);
    }
    else
    {
        w.string("speed", "max");
    }
    w.string("pacing", runner::pacing_name(state.pacing));
    w.begin_object("stop");
    w.integer("requests", state.stop_requests);
    optional_text(w, "by", state.stopped_by);
    w.end_object();
    w.boolean("scenario", state.scenario);
    w.boolean("handed_over", state.handed_over);
    if (state.scenario_over_at_ms)
    {
        w.integer("scenario_over_at_ms", *state.scenario_over_at_ms);
    }
    else
    {
        w.null("scenario_over_at_ms");
    }
    w.begin_object("allowed");
    write_allowed(w, "pause", state.allowed.pause);
    write_allowed(w, "resume", state.allowed.resume);
    write_allowed(w, "step", state.allowed.step, "min_ms", "max_ms", true);
    write_allowed(w, "speed", state.allowed.speed, "min", "max");
    write_allowed(w, "run_until", state.allowed.run_until);
    write_allowed(w, "stop", state.allowed.stop);
    write_allowed(w, "write", state.allowed.write);
    w.end_object();
    // What the run is connected to outside: one element for each bridge of --connect.
    w.begin_array("outside");
    for (const runner::RunState::Outside& o : state.outside)
    {
        w.begin_object({});
        w.string("network", o.network);
        w.string("target", o.target);
        w.string("participant", o.participant);
        optional_text(w, "link", o.link);
        optional_text(w, "bus", o.bus);
        w.boolean("nothing_received", o.nothing_received);
        w.string("condition", o.condition);
        w.begin_array("reasons");
        for (const std::string& reason : o.reasons)
        {
            w.string({}, reason);
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();
}

} // namespace

void detail::write_status_members(Writer& w, const ExecutionStatus& status)
{
    w.string("instance", status.instance);
    w.integer("status_rev", static_cast<std::int64_t>(status.status_rev));
    w.string("launcher_state", launcher_state_name(status.launcher_state));
    optional_text(w, "config", status.config);
    w.begin_object("eligibility");
    w.boolean("eligible", status.eligibility.eligible);
    optional_text(w, "reason", status.eligibility.reason);
    optional_text(w, "message", status.eligibility.message);
    w.end_object();
    w.begin_object("launcher_allowed");
    write_allowed(w, "prepare", status.launcher_allowed.prepare);
    write_allowed(w, "run", status.launcher_allowed.run);
    w.end_object();
    if (status.active_run_id)
    {
        w.string("active_run_id", *status.active_run_id);
    }
    else
    {
        w.null("active_run_id");
    }
    if (status.last_run_id)
    {
        w.string("last_run_id", *status.last_run_id);
    }
    else
    {
        w.null("last_run_id");
    }
    if (status.run_state)
    {
        w.begin_object("run_state");
        write_run_state_members(w, *status.run_state);
        w.end_object();
    }
    else
    {
        w.null("run_state");
    }
}

void EventStream::prepare(const PrepareResult& result, std::optional<std::int64_t> id)
{
    emit("prepare", object([&](Writer& w) {
             if (id)
             {
                 w.integer("id", *id);
             }
             else
             {
                 w.null("id");
             }
             w.boolean("accepted", result.accepted);
             optional_text(w, "reason", result.accepted ? std::string{} : result.reason);
             optional_text(w, "message", result.accepted ? std::string{} : result.message);
             w.begin_object("status");
             detail::write_status_members(w, result.status);
             w.end_object();
             if (result.accepted)
             {
                 w.begin_object("metadata");
                 w.begin_array("participants");
                 for (const std::string& p : result.metadata.participant_names)
                 {
                     w.string({}, p);
                 }
                 w.end_array();
                 w.begin_array("networks");
                 for (const std::string& n : result.metadata.network_names)
                 {
                     w.string({}, n);
                 }
                 w.end_array();
                 w.begin_array("variants");
                 for (const std::string& v : result.metadata.variants)
                 {
                     w.string({}, v);
                 }
                 w.end_array();
                 optional_text(w, "file_sha256", result.metadata.file_sha256);
                 w.end_object();
             }
             else
             {
                 w.null("metadata");
             }
             detail::write_findings_json(w, result.status.config, result.findings);
         }));
}

void EventStream::status(const ExecutionStatus& status, std::optional<std::int64_t> id)
{
    emit("status", object([&](Writer& w) {
             if (id)
             {
                 w.integer("id", *id);
             }
             else
             {
                 w.null("id");
             }
             detail::write_status_members(w, status);
         }));
}

void EventStream::state(const runner::RunState& state)
{
    emit("state", object([&](Writer& w) { write_run_state_members(w, state); }), Kind::State);
}

void EventStream::enqueue_result(std::string members, bool seal)
{
    std::uint64_t order = 0;
    const Kind kind = (window_ && seal) ? Kind::ResultFinal : Kind::Result;
    if (emit("result", std::move(members), kind, {}, &order))
    {
        result_order_ = order;
        result_enqueued_ = true;
    }
    else
    {
        result_order_ = 0;
        result_enqueued_ = false;
    }
}

bool EventStream::wait_result(const std::function<bool()>& give_up, bool seal)
{
    if (!result_enqueued_)
    {
        return false;
    }
    const std::uint64_t order = result_order_;
    // The stream's result (or a window's result that seals the stream) is the last line; an
    // unsealed window's result is written when the writer has come past it.
    const auto written = [this, order, seal](const State& s) { return (window_ && !seal) ? s.done_order >= order : s.finished; };
    State& s = *state_;
    std::unique_lock lock(s.mutex);
    // The limit of this wait is its own: a reader that reads, but too slowly, is no reader that
    // does not read (the watch's limit).
    const auto start = Clock::now();
    const auto deadline = start + s.options.stall_timeout;
    while (!written(s) && !s.closed)
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
            if (over && !written(s))
            {
                s.close("the result was not read, and the wait for its reader was stopped");
                break;
            }
        }
        s.changed.wait_for(lock, std::chrono::milliseconds(10));
    }
    return written(s);
}

bool EventStream::result(std::string members, const std::function<bool()>& give_up, bool seal)
{
    enqueue_result(std::move(members), seal);
    return wait_result(give_up, seal);
}

} // namespace vehicle_sil::launch
