// SPDX-License-Identifier: MIT
//
// The machine interface of vehicle_sil_launch (events.hpp): the event lines, their order and
// sequence numbers under several threads, nothing after the result; a reader that has gone, that
// does not read (no maker of events waits; "log" events are dropped and counted, nothing else;
// the stream closes after its limit) or reads late; the heartbeat; the commands on standard
// input; version --json.
#include "launch_test_util.hpp"
#include "silkit_test_util.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/launch/command_input.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/execution_runtime.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;
using vehicle_sil::config::test::check_text;

namespace {

/// The output of a stream and its reader: what was written; a reader that does not read (block),
/// an output that does not block and is full (busy), a reader that has gone (broken).
class Sink
{
public:
    ~Sink()
    {
        unblock(); // the writer of the stream is not left in a write
        stream_.reset();
    }

    /// Without heartbeats, and "t_ms" 7.
    static EventStreamOptions quiet()
    {
        EventStreamOptions options;
        options.heartbeat = 0ms;
        options.now_ms = [] { return std::int64_t{7}; };
        return options;
    }

    EventStream& start(EventStreamOptions options = quiet())
    {
        stream_ = std::make_unique<EventStream>(
            [s = state_](std::string_view bytes) {
                std::unique_lock lock(s->mutex);
                s->inside = true;
                s->changed.notify_all();
                s->changed.wait(lock, [&s] { return !s->blocked || s->allowed > 0; });
                s->allowed -= s->blocked ? 1 : 0;
                s->inside = false;
                EventWrite out;
                if (s->broken)
                {
                    out.failed = true;
                    out.error = "the test's output is closed";
                }
                else if (s->busy)
                {
                    lock.unlock();
                    std::this_thread::sleep_for(1ms);
                }
                else
                {
                    if (s->slow)
                    {
                        lock.unlock();
                        std::this_thread::sleep_for(50ms);
                        lock.lock();
                    }
                    s->bytes.append(bytes);
                    out.bytes = bytes.size();
                    ++s->writes;
                }
                s->changed.notify_all();
                return out;
            },
            [s = state_](const std::string& reason) {
                const std::lock_guard lock(s->mutex);
                s->closed.push_back(reason);
                s->closed_on = std::this_thread::get_id();
                s->changed.notify_all();
            },
            std::move(options));
        return *stream_;
    }

    void block() { set(&State::blocked, true); }
    /// While blocked: lets `n` more writes through, and waits until they are done and the writer
    /// is in the next one.
    bool allow(int n)
    {
        std::unique_lock lock(state_->mutex);
        const int target = state_->writes + n;
        state_->allowed = n;
        state_->changed.notify_all();
        return state_->changed.wait_for(lock, 10s, [&] { return state_->writes == target && state_->inside; });
    }
    void unblock() { set(&State::blocked, false); }
    /// Destroys the stream and waits for its writer and watch threads to exit.
    void stop() { stream_.reset(); }
    void set_busy(bool busy) { set(&State::busy, busy); }
    void break_it() { set(&State::broken, true); }
    /// Every write takes 50 ms.
    void set_slow(bool slow) { set(&State::slow, slow); }

    /// Waits until the writer is in a write that does not return (block()).
    bool wait_inside()
    {
        std::unique_lock lock(state_->mutex);
        return state_->changed.wait_for(lock, 10s, [this] { return state_->inside; });
    }

    /// Waits until the caller has been told that the stream closed; the reasons.
    std::vector<std::string> wait_closed(std::chrono::milliseconds timeout = 10s)
    {
        std::unique_lock lock(state_->mutex);
        state_->changed.wait_for(lock, timeout, [this] { return !state_->closed.empty(); });
        return state_->closed;
    }

    std::vector<std::string> closed()
    {
        const std::lock_guard lock(state_->mutex);
        return state_->closed;
    }

    std::thread::id closed_on()
    {
        const std::lock_guard lock(state_->mutex);
        return state_->closed_on;
    }

    std::string text()
    {
        const std::lock_guard lock(state_->mutex);
        return state_->bytes;
    }

    std::vector<std::string> lines()
    {
        const std::string all = text();
        std::vector<std::string> out;
        std::size_t from = 0;
        for (auto end = all.find('\n'); end != std::string::npos; end = all.find('\n', from))
        {
            out.push_back(all.substr(from, end - from));
            from = end + 1;
        }
        CHECK(from == all.size()); // every line is whole
        return out;
    }

    int writes()
    {
        const std::lock_guard lock(state_->mutex);
        return state_->writes;
    }

private:
    struct State
    {
        std::mutex mutex;
        std::condition_variable changed;
        std::string bytes;
        int writes = 0;
        bool blocked = false;
        int allowed = 0; ///< writes that pass although it is blocked
        bool busy = false;
        bool broken = false;
        bool slow = false;
        bool inside = false;
        std::vector<std::string> closed;
        std::thread::id closed_on;
    };

    void set(bool State::* what, bool value)
    {
        const std::lock_guard lock(state_->mutex);
        (*state_).*what = value;
        state_->changed.notify_all();
    }

    // Shared with the stream's threads: one that is still in a write keeps it.
    std::shared_ptr<State> state_ = std::make_shared<State>();
    std::unique_ptr<EventStream> stream_;
};

/// The value of "seq" in an event line.
std::int64_t seq_of(const std::string& line)
{
    const auto at = line.find("\"seq\":");
    return at == std::string::npos ? -1 : std::stoll(line.substr(at + 6));
}

/// The lines without the head every line has, "<event> <members>" each: what is written, apart
/// from "seq" (checked to count from 0 without a gap) and "t_ms".
std::string shape(const std::vector<std::string>& lines)
{
    std::string out;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        const std::string& line = lines[i];
        CHECK_EQ(seq_of(line), static_cast<std::int64_t>(i));
        const auto name = line.find("\"event\":\"") + 9;
        const auto members = line.find_first_of(",}", line.find("\"t_ms\":"));
        out += line.substr(name, line.find('"', name) - name) + " " + line.substr(members + 1) + "\n";
    }
    return out;
}

Command stop_command()
{
    Command c;
    c.name = "stop";
    c.accepted = true;
    return c;
}

template <class Call>
std::chrono::milliseconds time_of(Call call)
{
    const auto from = std::chrono::steady_clock::now();
    call();
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - from);
}

} // namespace

TEST_CASE(each_event_is_one_line_with_event_seq_and_t_ms_first)
{
    Sink sink;
    EventStream& e = sink.start();
    e.hello(42, "dir/sim.yaml", {"run", "--events", "dir/sim.yaml"});
    e.phase(Phase::Checking);
    e.registry("silkit://127.0.0.1:8500");
    e.participant("Logger", ParticipantState::Disabled);
    e.participant_not_started("Bad", "cannot start ./bad: No such file");
    e.participant_started("Engine", 99, {"/x/engine", "--name", "Engine"}, "");
    e.participant_started("Logged", 98, {"/x/l"}, "logs/Logged.log");
    e.participant("Engine", ParticipantState::Joined);
    e.participant_failed("Engine", "left_simulation", "");
    e.participant_failed("Crash", "process_ended", "participant 'Crash': ...");
    e.time(TimeEvent{.sim_ms = 1000});
    e.time(TimeEvent{.sim_ms = 1010, .actual_speed = 0.48, .late_steps = 3, .max_late_ms = 12,
                     .slipped_ms = 30, .late_ms = 4});
    // A participant's line may hold anything: quotes, a backslash, a tab, a line end.
    e.log("Engine", "say \"hi\"\\\tthen\nmore");
    e.outside("ETH_EXT", 152, 0);
    e.participant_ended("Engine", process::ExitStatus{0, std::nullopt});
    e.participant_ended("Crash", process::ExitStatus{139, 11});
    e.participant("Deaf", ParticipantState::StopRequested);
    e.participant("Deaf", ParticipantState::Killed);
    e.finding("sim.yaml", Finding{Severity::Note, "interrupted", "interrupted (stop command): ...", 0, "", ""});
    e.finding("sim.yaml", Finding{Severity::Error, "exit-code", "participant 'A': ...", 5, "A", "", "more"});
    Command bad;
    bad.message = "the line is not a JSON object";
    e.command(bad, "stdin");
    e.command(stop_command(), "eof");
    e.phase(Phase::Ended);
    CHECK(e.wait_written(10s));
    const std::string v = version();
    check_text(sink.text(),
               R"({"event":"hello","seq":0,"t_ms":7,"vehicle_sil_events":2,"vehicle_sil":")" + v +
                   R"(","pid":42,"file":"dir/sim.yaml","arguments":["run","--events","dir/sim.yaml"]}
{"event":"phase","seq":1,"t_ms":7,"phase":"checking"}
{"event":"registry","seq":2,"t_ms":7,"uri":"silkit://127.0.0.1:8500"}
{"event":"participant","seq":3,"t_ms":7,"name":"Logger","state":"disabled"}
{"event":"participant","seq":4,"t_ms":7,"name":"Bad","state":"not_started","message":"cannot start ./bad: No such file"}
{"event":"participant","seq":5,"t_ms":7,"name":"Engine","state":"started","pid":99,"command":["/x/engine","--name","Engine"],"log":null}
{"event":"participant","seq":6,"t_ms":7,"name":"Logged","state":"started","pid":98,"command":["/x/l"],"log":"logs/Logged.log"}
{"event":"participant","seq":7,"t_ms":7,"name":"Engine","state":"joined"}
{"event":"participant","seq":8,"t_ms":7,"name":"Engine","state":"failed","reason":"left_simulation"}
{"event":"participant","seq":9,"t_ms":7,"name":"Crash","state":"failed","reason":"process_ended","message":"participant 'Crash': ..."}
{"event":"time","seq":10,"t_ms":7,"sim_ms":1000,"actual_speed":null,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":11,"t_ms":7,"sim_ms":1010,"actual_speed":0.48,"late_steps":3,"max_late_ms":12,"slipped_ms":30,"late_ms":4}
{"event":"log","seq":12,"t_ms":7,"source":"Engine","line":"say \"hi\"\\\tthen\nmore"}
{"event":"outside","seq":13,"t_ms":7,"network":"ETH_EXT","to_outside":152,"from_outside":0}
{"event":"participant","seq":14,"t_ms":7,"name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"participant","seq":15,"t_ms":7,"name":"Crash","state":"ended","exit_code":null,"signal":")" +
                   process::signal_name(11) + R"("}
{"event":"participant","seq":16,"t_ms":7,"name":"Deaf","state":"stop_requested"}
{"event":"participant","seq":17,"t_ms":7,"name":"Deaf","state":"killed"}
{"event":"finding","seq":18,"t_ms":7,"severity":"note","code":"interrupted","file":"sim.yaml","line":null,"participant":null,"network":null,"message":"interrupted (stop command): ...","detail":null}
{"event":"finding","seq":19,"t_ms":7,"severity":"error","code":"exit-code","file":"sim.yaml","line":5,"participant":"A","network":null,"message":"participant 'A': ...","detail":"more"}
{"event":"command","seq":20,"t_ms":7,"command":null,"source":"stdin","accepted":false,"message":"the line is not a JSON object","reason":null,"id":null}
{"event":"command","seq":21,"t_ms":7,"command":"stop","source":"eof","accepted":true,"message":null,"reason":null,"id":null}
{"event":"phase","seq":22,"t_ms":7,"phase":"ended"}
)");
    CHECK(sink.closed().empty() && !e.closed());
}

TEST_CASE(the_check_and_the_result_are_their_json_on_one_line_and_nothing_follows_the_result)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.heartbeat = 10ms; // nor a heartbeat
    EventStream& e = sink.start(options);
    Checked check;
    check.file = "x.yaml";
    check.findings.push_back(Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""});
    check.scenario.emplace();
    check.scenario->file = "drive.yaml";
    e.check(check);
    RunReport report;
    report.check = check;
    // The result is written when result() returns.
    CHECK(e.result(format_result_event(report)));
    CHECK(sink.lines().size() >= 2 && sink.lines().back().find("\"event\":\"result\"") != std::string::npos);
    // The scenario of the check is in the event "check".
    const std::vector<std::string> written = sink.lines(); // a copy: begin() and end() of one vector
    CHECK(std::any_of(written.begin(), written.end(), [](const std::string& line) {
        return line.find("\"event\":\"check\"") != std::string::npos && line.find("\"scenario\":{\"file\":\"drive.yaml\"") != std::string::npos;
    }));
    const std::size_t count = sink.lines().size();
    e.log("late", "after the result");
    e.phase(Phase::Ended);
    CHECK(!e.result(format_result_event(report))); // only one result
    sink.stop();
    CHECK(sink.lines().size() == count);
    CHECK(sink.closed().empty());
    // The same JSON as check --json and run --json, without its line ends and spaces.
    const auto compact = [](const std::string& pretty) {
        std::string out;
        bool inString = false;
        for (std::size_t i = 0; i < pretty.size(); ++i)
        {
            const char c = pretty[i];
            if (inString)
            {
                out += c;
                if (c == '\\')
                {
                    out += pretty[++i];
                }
                else if (c == '"')
                {
                    inString = false;
                }
            }
            else if (c == '"')
            {
                inString = true;
                out += c;
            }
            else if (c != ' ' && c != '\n')
            {
                out += c;
            }
        }
        return out;
    };
    const auto lines = sink.lines();
    check_text(lines.front(), R"({"event":"check","seq":0,"t_ms":7,"check":)" + compact(format_json(check)) + "}");
    check_text(lines.back(), R"({"event":"result","seq":)" + std::to_string(count - 1) + R"(,"t_ms":7,"result":)" +
                                 compact(format_run_json(report)) + "}");
}

TEST_CASE(seq_follows_the_order_of_the_lines_whatever_thread_writes)
{
    Sink sink;
    EventStream& e = sink.start();
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&e, t] {
            for (int i = 0; i < 500; ++i)
            {
                // No "log": every one of these is written, however far the writer is behind.
                e.participant("P" + std::to_string(t) + " " + std::to_string(i), ParticipantState::Joined);
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK(e.wait_written(30s));
    const auto lines = sink.lines();
    CHECK(lines.size() == 2000);
    std::vector<int> next(4, 0);
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        CHECK_EQ(seq_of(lines[i]), static_cast<std::int64_t>(i));
        // Each thread's events in the order of its calls.
        const auto at = lines[i].find("\"name\":\"P") + 9;
        const int thread = lines[i][at] - '0';
        CHECK_EQ(std::stoi(lines[i].substr(at + 2)), next[static_cast<std::size_t>(thread)]++);
    }
}

TEST_CASE(a_reader_that_went_away_closes_the_stream_and_the_caller_is_told_once_by_the_watch)
{
    Sink sink;
    EventStream& e = sink.start();
    e.phase(Phase::Running);
    CHECK(e.wait_written(10s));
    sink.break_it();
    e.log("Engine", "lost");
    const auto closed = sink.wait_closed();
    CHECK(closed.size() == 1 && e.closed());
    check_text(closed.at(0), "the test's output is closed");
    check_text(e.close_reason(), "the test's output is closed");
    // Not on the thread that made the event: what the caller does then (a line on stderr) may wait.
    CHECK(sink.closed_on() != std::this_thread::get_id());
    e.log("Engine", "not written");
    e.phase(Phase::Stopping);
    CHECK(!e.wait_written(0ms));
    CHECK(!e.result(format_result_event(RunReport{})));
    sink.stop();
    CHECK(sink.closed().size() == 1);
    check_text(shape(sink.lines()), "phase \"phase\":\"running\"}\n");
}

TEST_CASE(wait_on_closed_waits_for_on_closed_callback_with_a_timeout)
{
    // 1. Open stream: returns true immediately.
    {
        Sink sink;
        EventStream& e = sink.start();
        CHECK(e.wait_on_closed(0ms));
    }
    // 2. Closed before result(): waits until on_closed returns (or timeout expires).
    {
        struct Shared
        {
            std::mutex mu;
            std::condition_variable cv;
            bool entered = false;
            bool release = false;
            std::atomic<bool> note_written{false};
            std::atomic<bool> fail_write{false};
        };
        auto st = std::make_shared<Shared>();

        EventStreamOptions options = Sink::quiet();
        EventStream e(
            [st](std::string_view bytes) {
                EventWrite out;
                if (st->fail_write.load())
                {
                    out.failed = true;
                    out.error = "broken pipe";
                }
                else
                {
                    out.bytes = bytes.size();
                }
                return out;
            },
            [st](const std::string&) {
                {
                    const std::lock_guard lock(st->mu);
                    st->entered = true;
                }
                st->cv.notify_all();
                std::unique_lock lock(st->mu);
                st->cv.wait(lock, [&] { return st->release; });
                st->note_written.store(true);
            },
            options);

        e.phase(Phase::Running);
        CHECK(e.wait_written(10s));
        st->fail_write.store(true);
        e.log("Engine", "triggers close");
        {
            std::unique_lock lock(st->mu);
            CHECK(st->cv.wait_for(lock, 10s, [&] { return st->entered; }));
        }
        CHECK(e.closed());
        // While on_closed is still running, wait_on_closed respects the timeout.
        CHECK(!e.wait_on_closed(30ms));
        CHECK(!st->note_written.load());
        // Once on_closed is allowed to finish, wait_on_closed observes its completion.
        std::thread releaser([st] {
            std::this_thread::sleep_for(20ms);
            {
                const std::lock_guard lock(st->mu);
                st->release = true;
            }
            st->cv.notify_all();
        });
        CHECK(e.wait_on_closed(10s));
        CHECK(st->note_written.load());
        releaser.join();
    }
    // 3. Closed after result() sealed the stream (on_closed not called): returns true immediately.
    {
        Sink sink;
        EventStreamOptions options = Sink::quiet();
        options.stall_timeout = 100ms;
        EventStream& e = sink.start(options);
        sink.block();
        CHECK(!e.result(format_result_event(RunReport{})));
        CHECK(e.closed());
        CHECK(e.wait_on_closed(0ms));
        CHECK(sink.closed().empty());
    }
}

TEST_CASE(no_maker_of_events_waits_for_a_reader_that_does_not_read_and_only_log_events_are_dropped)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_log_lines = 3;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running); // the writer is in this line
    CHECK(sink.wait_inside());
    const auto took = time_of([&e] {
        e.log("A", "1");
        e.log("B", "2");
        e.log("A", "3"); // the queue's limit
        e.log("A", "4");
        e.log("B", "5");
        e.log("A", "6");
        // Not a "log": never dropped, and it does not wait either.
        e.participant("A", ParticipantState::Joined);
        e.log("A", "7");
        e.time(TimeEvent{.sim_ms = 5});
    });
    CHECK(took < 1000ms);
    CHECK(!e.closed() && sink.text().empty());
    sink.unblock();
    CHECK(e.wait_written(10s));
    // There is room again: lines are taken, and what is dropped later is counted at its own place.
    e.log("B", "8");
    CHECK(e.wait_written(10s));
    sink.block();
    e.phase(Phase::Stopping);
    CHECK(sink.wait_inside());
    for (int i = 9; i < 14; ++i)
    {
        e.log("C", std::to_string(i));
    }
    sink.unblock();
    CHECK(e.result(format_result_event(RunReport{})));
    std::string all = shape(sink.lines());
    all.erase(all.rfind("result ")); // the result's JSON: another test
    check_text(all, R"(phase "phase":"running"}
log "source":"A","line":"1"}
log "source":"B","line":"2"}
log "source":"A","line":"3"}
log_dropped "count":4,"sources":{"A":3,"B":1}}
participant "name":"A","state":"joined"}
time "sim_ms":5,"actual_speed":null,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
log "source":"B","line":"8"}
phase "phase":"stopping"}
log "source":"C","line":"9"}
log "source":"C","line":"10"}
log "source":"C","line":"11"}
log_dropped "count":2,"sources":{"C":2}}
)");
    CHECK(sink.closed().empty());
}

TEST_CASE(lines_dropped_after_a_line_that_was_taken_are_counted_at_a_place_of_their_own)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_log_lines = 3;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running);
    CHECK(sink.wait_inside());
    e.log("A", "1");
    e.log("A", "2");
    e.log("A", "3");
    e.log("A", "4"); // dropped: the first place, still waiting to be written
    // The reader takes two lines (the phase, 1) and is in the third (2): 3 waits, so there is
    // room for two "log" behind that place.
    CHECK(sink.allow(2));
    e.log("A", "5");
    e.log("A", "6");
    e.log("B", "7"); // dropped behind 6: a second place
    sink.unblock();
    CHECK(e.wait_written(10s));
    check_text(shape(sink.lines()), R"(phase "phase":"running"}
log "source":"A","line":"1"}
log "source":"A","line":"2"}
log "source":"A","line":"3"}
log_dropped "count":1,"sources":{"A":1}}
log "source":"A","line":"5"}
log "source":"A","line":"6"}
log_dropped "count":1,"sources":{"B":1}}
)");
}

TEST_CASE(a_log_event_as_large_as_the_limit_is_taken_when_none_waits)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_log_bytes = 64;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running);
    CHECK(sink.wait_inside());
    e.log("A", std::string(200, 'x')); // alone: taken
    e.log("A", "short");               // the bytes are over the limit
    sink.unblock();
    CHECK(e.wait_written(10s));
    const auto lines = sink.lines();
    CHECK(lines.size() == 3);
    CHECK(lines.at(1).find(std::string(200, 'x')) != std::string::npos);
    CHECK(lines.at(2).find("\"event\":\"log_dropped\"") != std::string::npos && lines.at(2).find("\"count\":1,") != std::string::npos);
}

TEST_CASE(the_stream_closes_when_too_many_events_that_cannot_be_dropped_wait)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_other_events = 5;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running);
    CHECK(sink.wait_inside());
    for (int i = 0; i < 5; ++i)
    {
        e.command(stop_command(), "stdin");
    }
    CHECK(!e.closed());
    e.command(stop_command(), "stdin");
    CHECK(e.closed());
    // The caller is told although the writer is still in its write.
    const auto closed = sink.wait_closed();
    CHECK(closed.size() == 1);
    check_text(closed.at(0), "more than 5 events wait to be written");
    sink.unblock();
    sink.stop();
    // The write that was waiting may end; nothing follows it.
    CHECK(sink.text().empty() || sink.text() == "{\"event\":\"phase\",\"seq\":0,\"t_ms\":7,\"phase\":\"running\"}\n");
}

namespace {

/// A state with `rev`, otherwise as a run that is being set up.
RunState state_of(std::uint64_t rev)
{
    RunState s;
    s.rev = rev;
    s.allowed.step = Allowed{false, "not-set-up", "the simulation has not been set up yet", std::nullopt,
                             ArgumentRange{1, static_cast<double>(kMaxMillis)}};
    s.allowed.speed.range = ArgumentRange{kMinSpeed, kMaxSpeed};
    return s;
}

} // namespace

TEST_CASE(a_state_event_has_the_state_and_what_is_allowed_with_the_ranges)
{
    Sink sink;
    EventStream& e = sink.start();
    // Each is written before the next is made: one that waits would be replaced.
    e.state(state_of(1));
    CHECK(e.wait_written(10s));
    RunState s = state_of(2);
    s.phase = Phase::Running;
    s.paused = true;
    s.paused_at_ms = 120;
    s.holding = true;
    s.speed = std::optional<double>(0.5);
    s.pacing = Pacing::Late;
    s.stop_requests = 2;
    s.stopped_by = "SIGINT";
    s.scenario = true;
    s.allowed.step = Allowed{true, "", "", std::nullopt, ArgumentRange{1, static_cast<double>(kMaxMillis)}};
    s.allowed.speed.range = ArgumentRange{1, 1};
    s.allowed.pause.caution = Caution{"some-code", "a caution"};
    s.allowed.write = Allowed{false, "scenario-running", "why \"not\"", std::nullopt, std::nullopt};
    e.state(s);
    CHECK(e.wait_written(10s));
    s.rev = 3;
    s.speed = std::optional<double>();
    s.pacing = Pacing::OnTime;
    e.state(s);
    CHECK(e.wait_written(10s));
    const std::vector<std::string> lines = sink.lines();
    CHECK_EQ(lines.size(), std::size_t{3});
    check_text(lines.at(0),
               R"({"event":"state","seq":0,"t_ms":7,"rev":1,"phase":"checking","paused":false,"paused_at_ms":null,"holding":false,"speed":null,"pacing":"not-paced","stop":{"requests":0,"by":null},"scenario":false,"allowed":{"pause":{"ok":true,"reason":null,"message":null,"caution":null},"resume":{"ok":true,"reason":null,"message":null,"caution":null},"step":{"ok":false,"reason":"not-set-up","message":"the simulation has not been set up yet","caution":null,"min_ms":1,"max_ms":1000000000000},"speed":{"ok":true,"reason":null,"message":null,"caution":null,"min":1e-06,"max":1e+06},"run_until":{"ok":true,"reason":null,"message":null,"caution":null},"stop":{"ok":true,"reason":null,"message":null,"caution":null},"write":{"ok":true,"reason":null,"message":null,"caution":null}},"outside":[]})");
    check_text(lines.at(1),
               R"({"event":"state","seq":1,"t_ms":7,"rev":2,"phase":"running","paused":true,"paused_at_ms":120,"holding":true,"speed":0.5,"pacing":"late","stop":{"requests":2,"by":"SIGINT"},"scenario":true,"allowed":{"pause":{"ok":true,"reason":null,"message":null,"caution":{"code":"some-code","message":"a caution"}},"resume":{"ok":true,"reason":null,"message":null,"caution":null},"step":{"ok":true,"reason":null,"message":null,"caution":null,"min_ms":1,"max_ms":1000000000000},"speed":{"ok":true,"reason":null,"message":null,"caution":null,"min":1,"max":1},"run_until":{"ok":true,"reason":null,"message":null,"caution":null},"stop":{"ok":true,"reason":null,"message":null,"caution":null},"write":{"ok":false,"reason":"scenario-running","message":"why \"not\"","caution":null}},"outside":[]})");
    CHECK(lines.at(2).find(R"("rev":3,"phase":"running","paused":true,"paused_at_ms":120,"holding":true,"speed":"max","pacing":"on-time",)") !=
          std::string::npos);
}

TEST_CASE(a_state_event_has_an_element_for_each_bridge_with_its_condition_and_reasons)
{
    Sink sink;
    EventStream& e = sink.start();
    RunState s = state_of(1);
    RunState::Outside eth;
    eth.network = "ETH_EXT";
    eth.target = "enp3s0";
    eth.participant = "bridge.ETH_EXT";
    eth.link = "up";
    eth.nothing_received = true;
    eth.condition = "degraded";
    eth.reasons = {"frames-dropped", "device-shared"};
    RunState::Outside can;
    can.network = "CAN";
    can.target = "can0";
    can.participant = "bridge.CAN";
    can.link = "down";
    can.bus = "bus-off";
    can.condition = "interrupted";
    can.reasons = {"link-down", "bus-off"};
    s.outside = {eth, can};
    CHECK(s != state_of(1));
    e.state(s);
    CHECK(e.wait_written(10s));
    const std::vector<std::string> lines = sink.lines();
    CHECK_EQ(lines.size(), std::size_t{1});
    CHECK(lines.at(0).find(R"("outside":[{"network":"ETH_EXT","target":"enp3s0","participant":"bridge.ETH_EXT","link":"up","bus":null,"nothing_received":true,"condition":"degraded","reasons":["frames-dropped","device-shared"]},{"network":"CAN","target":"can0","participant":"bridge.CAN","link":"down","bus":"bus-off","nothing_received":false,"condition":"interrupted","reasons":["link-down","bus-off"]}]})") !=
          std::string::npos);
}

TEST_CASE(only_the_latest_state_waits_and_states_never_close_the_stream)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_other_events = 5;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running); // the writer is in this line
    CHECK(sink.wait_inside());
    // Far more states than the events that would close the stream: none of them counts.
    e.participant("A", ParticipantState::Joined);
    for (std::uint64_t rev = 1; rev <= 10000; ++rev)
    {
        e.state(state_of(rev));
    }
    CHECK(!e.closed());
    CHECK_EQ(e.waiting_events(), std::size_t{1});
    // History made after a state goes after it; the next state takes the waiting one's place and
    // goes to the end, after that history.
    e.participant("B", ParticipantState::Joined);
    e.state(state_of(10001));
    e.participant("C", ParticipantState::Joined);
    CHECK_EQ(e.waiting_events(), std::size_t{3});
    // The other events still close it at their limit, states or not.
    CHECK(!e.closed());
    sink.unblock();
    CHECK(e.wait_written(10s));
    std::string all;
    for (const std::string& line : sink.lines())
    {
        const auto name = line.find("\"event\":\"") + 9;
        all += line.substr(name, line.find('"', name) - name);
        if (const auto rev = line.find("\"rev\":"); rev != std::string::npos)
        {
            all += " " + line.substr(rev + 6, line.find(',', rev) - rev - 6);
        }
        else if (const auto who = line.find("\"name\":\""); who != std::string::npos)
        {
            all += " " + line.substr(who + 8, 1);
        }
        all += "\n";
    }
    // One state, the last, with the seq of its place: after everything made before it.
    check_text(all, "phase\nparticipant A\nparticipant B\nstate 10001\nparticipant C\n");
    CHECK_EQ(seq_of(sink.lines().at(3)), std::int64_t{3});
    // A state that is being written is written; the next one follows it.
    sink.block();
    e.state(state_of(10002));
    CHECK(sink.wait_inside());
    e.state(state_of(10003));
    e.state(state_of(10004));
    sink.unblock();
    CHECK(e.wait_written(10s));
    CHECK_EQ(sink.lines().size(), std::size_t{7});
    CHECK(sink.lines().at(5).find("\"rev\":10002,") != std::string::npos);
    CHECK(sink.lines().at(6).find("\"rev\":10004,") != std::string::npos);
    // Nothing follows the result, a state neither.
    CHECK(e.result(format_result_event(RunReport{})));
    e.state(state_of(10005));
    CHECK(e.wait_written(10s));
    CHECK_EQ(sink.lines().size(), std::size_t{8});
    CHECK(sink.lines().back().find("\"event\":\"result\"") != std::string::npos);
    CHECK(sink.closed().empty());
}

TEST_CASE(a_state_that_waits_leaves_room_for_exactly_as_many_other_events_as_without_it)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.max_other_events = 3;
    EventStream& e = sink.start(options);
    sink.block();
    e.phase(Phase::Running);
    CHECK(sink.wait_inside());
    e.state(state_of(1));
    for (int i = 0; i < 3; ++i)
    {
        e.command(stop_command(), "stdin");
        e.state(state_of(static_cast<std::uint64_t>(i + 2)));
    }
    CHECK(!e.closed());
    e.command(stop_command(), "stdin");
    CHECK(e.closed());
    check_text(sink.wait_closed().at(0), "more than 3 events wait to be written");
    // A closed stream takes no state.
    e.state(state_of(9));
    CHECK_EQ(e.waiting_events(), std::size_t{0});
}

TEST_CASE(a_reader_that_does_not_read_is_given_up_after_the_limit)
{
    for (const bool busy : {false, true})
    {
        Sink sink;
        EventStreamOptions options = Sink::quiet();
        options.stall_timeout = 150ms;
        EventStream& e = sink.start(options);
        e.phase(Phase::Checking);
        CHECK(e.wait_written(10s));
        // Nothing waits: no limit runs.
        std::this_thread::sleep_for(400ms);
        CHECK(!e.closed());
        // A write that does not return, or an output that takes nothing.
        busy ? sink.set_busy(true) : sink.block();
        const auto from = std::chrono::steady_clock::now();
        e.phase(Phase::Running);
        const auto closed = sink.wait_closed();
        const auto after = std::chrono::steady_clock::now() - from;
        CHECK(closed.size() == 1 && e.closed());
        check_text(closed.at(0), "their reader has not read for 150 ms");
        CHECK(after >= 150ms && after < 5s);
        CHECK(time_of([&e] { e.log("A", "x"); }) < 1000ms);
        CHECK(!e.result(format_result_event(RunReport{})));
    }
    // A reader that reads slowly is not given up: every write that is taken starts the limit anew.
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.stall_timeout = 300ms;
    EventStream& e = sink.start(options);
    for (int i = 0; i < 6; ++i)
    {
        sink.block();
        e.registry("silkit://" + std::to_string(i));
        CHECK(sink.wait_inside());
        std::this_thread::sleep_for(100ms);
        sink.unblock();
        CHECK(e.wait_written(10s));
    }
    CHECK(!e.closed() && sink.lines().size() == 6);
}

TEST_CASE(a_heartbeat_is_written_when_nothing_else_is_from_the_first_line_to_the_result)
{
    Sink sink;
    EventStreamOptions options;
    options.heartbeat = 30ms;
    EventStream& e = sink.start(options);
    std::this_thread::sleep_for(150ms);
    check_text(sink.text(), ""); // hello is the first line
    e.hello(1, "", {});
    std::this_thread::sleep_for(250ms);
    e.phase(Phase::Checking);
    CHECK(e.result(format_result_event(RunReport{})));
    const auto lines = sink.lines();
    std::this_thread::sleep_for(150ms);
    CHECK(sink.lines().size() == lines.size()); // nothing after the result
    int beats = 0;
    std::int64_t before = 0;
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        CHECK_EQ(seq_of(lines[i]), static_cast<std::int64_t>(i));
        const auto at = lines[i].find("\"t_ms\":");
        const std::int64_t t = std::stoll(lines[i].substr(at + 7));
        CHECK(t >= before); // the time never goes back
        before = t;
        if (lines[i].rfind("{\"event\":\"heartbeat\",", 0) == 0)
        {
            ++beats;
            // Nothing but the head.
            check_text(lines[i], "{\"event\":\"heartbeat\",\"seq\":" + std::to_string(i) + ",\"t_ms\":" + std::to_string(t) + "}");
        }
    }
    CHECK(lines.front().find("\"event\":\"hello\"") != std::string::npos);
    CHECK(lines.back().find("\"event\":\"result\"") != std::string::npos);
    // About 8 in 250 ms; fewer on a machine under load, never none and never one per write.
    CHECK(beats >= 1 && beats <= 12);
}

TEST_CASE(the_result_waits_to_be_written_but_not_for_a_reader_that_does_not_read)
{
    // Up to the limit.
    {
        Sink sink;
        EventStreamOptions options = Sink::quiet();
        options.stall_timeout = 200ms;
        EventStream& e = sink.start(options);
        sink.block();
        bool written = true;
        const auto took = time_of([&] { written = e.result(format_result_event(RunReport{})); });
        CHECK(!written && took >= 200ms && took < 5s);
        CHECK(e.closed());
        CHECK(e.close_reason() == "the result could not be written within 200 ms" ||
              e.close_reason() == "their reader has not read for 200 ms");
        // The run is over: there is nothing to stop, so the caller is not told through on_closed.
        std::this_thread::sleep_for(50ms);
        CHECK(sink.closed().empty());
    }
    // A reader that reads, but too slowly for what waits: the same limit.
    {
        Sink sink;
        EventStreamOptions options = Sink::quiet();
        options.stall_timeout = 300ms;
        EventStream& e = sink.start(options);
        sink.set_slow(true);
        for (int i = 0; i < 40; ++i)
        {
            e.registry("silkit://" + std::to_string(i));
        }
        bool written = true;
        const auto took = time_of([&] { written = e.result(format_result_event(RunReport{})); });
        CHECK(!written && took >= 300ms && took < 1500ms);
        check_text(e.close_reason(), "the result could not be written within 300 ms");
        const auto count = sink.lines().size();
        CHECK(count >= 1 && count < 40);
        std::this_thread::sleep_for(150ms);
        CHECK(sink.lines().size() <= count + 1); // at most the line that was being written
    }
    // Until the caller gives up.
    {
        Sink sink;
        EventStream& e = sink.start();
        sink.block();
        std::atomic<int> asked{0};
        bool written = true;
        const auto took = time_of(
            [&] { written = e.result(format_result_event(RunReport{}), [&asked] { return ++asked >= 3; }); });
        // Asked only once the reader has not read for a moment: a reader that reads gets the result.
        CHECK(!written && took >= 500ms && took < 5s && asked.load() == 3);
        check_text(e.close_reason(), "the result was not read, and the wait for its reader was stopped");
        CHECK(sink.closed().empty());
    }
    // A reader that reads gets the result, whatever the caller would answer.
    {
        Sink sink;
        EventStream& e = sink.start();
        for (int i = 0; i < 1000; ++i)
        {
            e.registry("silkit://" + std::to_string(i));
        }
        CHECK(e.result(format_result_event(RunReport{}), [] { return true; }));
        CHECK(sink.lines().size() == 1001 && !e.closed());
    }
    // A reader that reads in time gets it, however late it started.
    {
        Sink sink;
        EventStreamOptions options = Sink::quiet();
        options.stall_timeout = 5000ms;
        EventStream& e = sink.start(options);
        sink.block();
        e.phase(Phase::Ended);
        CHECK(sink.wait_inside());
        bool written = false;
        std::thread caller([&] { written = e.result(format_result_event(RunReport{})); });
        // Let "phase" finish and wait until the writer blocks inside writing "result" itself.
        CHECK(sink.allow(1));
        sink.unblock();
        caller.join();
        CHECK(written && !e.closed());
        CHECK(sink.lines().size() == 2);
    }
}

TEST_CASE(a_command_is_a_json_object_with_the_command_stop)
{
    const auto read = [](std::string_view line) {
        const auto c = parse_command(line);
        if (!c)
        {
            return std::string("(skipped)");
        }
        return std::string(c->accepted ? "accepted " : "refused ") + (c->name ? "'" + *c->name + "'" : "-") + ": " +
               c->message;
    };
    check_text(read(R"({"command":"stop"})"), "accepted 'stop': ");
    check_text(read(R"(  { "command" : "stop", "later": [1, {"x": null}] }  )" "\r"), "accepted 'stop': ");
    CHECK(parse_command(R"({"command":"stop"})")->is_stop());
    check_text(read(""), "(skipped)");
    check_text(read(" \t\r"), "(skipped)");
    check_text(read("stop"), "refused -: the line is not a JSON object: write one JSON object per line, such as "
                             "{\"command\":\"stop\"}");
    check_text(read(R"(["stop"])"), "refused -: the line is not a JSON object: write one JSON object per line, such as "
                                    "{\"command\":\"stop\"}");
    check_text(read(R"({"command":"stop")"), "refused -: the line is not valid JSON: write one JSON object per line, "
                                             "such as {\"command\":\"stop\"}");
    check_text(read(R"({"cmd":"stop"})"), "refused -: the object has no \"command\": write one JSON object per line, "
                                          "such as {\"command\":\"stop\"}");
    check_text(read(R"({"command":1})"), "refused -: \"command\" is not a string: write one JSON object per line, such "
                                         "as {\"command\":\"stop\"}");
    check_text(read(R"({"command":{"name":"stop"}})"), "refused -: \"command\" is not a string: write one JSON object "
                                                       "per line, such as {\"command\":\"stop\"}");
    check_text(read(R"({"command":"jump"})"),
               "refused 'jump': unknown command 'jump': the commands are stop, pause, resume, step, run_until and speed");
    CHECK(!parse_command(R"({"command":"jump"})")->is_stop());
    // Each refusal has its code; an accepted line has none.
    const auto reason = [](std::string_view line) { return parse_command(line)->reason; };
    check_text(reason(R"({"command":"stop"})"), "");
    check_text(reason("stop"), "invalid-command");
    check_text(reason(R"(["stop"])"), "invalid-command");
    check_text(reason(R"({"command":"stop")"), "invalid-command");
    check_text(reason(R"({"cmd":"stop"})"), "invalid-command");
    check_text(reason(R"({"command":1})"), "invalid-command");
    check_text(reason(R"({"command":"jump"})"), "unknown-command");
    check_text(reason(R"({"command":"step"})"), "invalid-argument");
    check_text(reason(R"({"command":"speed","value":0})"), "invalid-argument");
}

TEST_CASE(a_command_may_carry_the_senders_number_which_comes_back_in_its_event)
{
    // Every command takes an "id"; without one there is none.
    CHECK(!parse_command(R"({"command":"pause"})")->id);
    for (const char* line : {R"({"command":"pause","id":0})", R"({"id":0,"command":"stop"})",
                             R"({"command":"step","ms":10,"id":0})", R"({"command":"speed","value":"max","id":0})"})
    {
        const Command c = *parse_command(line);
        CHECK(c.accepted);
        CHECK(c.id == std::optional<std::int64_t>(0));
    }
    CHECK(parse_command(R"({"command":"resume","id":9007199254740992})")->id ==
          std::optional<std::int64_t>(std::int64_t{9007199254740992}));
    // An "id" that is no such number: the command is refused, so that its sender is told.
    for (const char* line :
         {R"({"command":"pause","id":-1})", R"({"command":"pause","id":9007199254740993})", R"({"command":"pause","id":"1"})",
          R"({"command":"pause","id":1.5})", R"({"command":"pause","id":null})", R"({"command":"pause","id":[1]})",
          R"({"command":"pause","id":{"n":1}})", R"({"command":"pause","id":true})"})
    {
        const Command c = *parse_command(line);
        CHECK(!c.accepted);
        CHECK(!c.id);
        check_text(c.reason, "invalid-argument");
        check_text(c.message, "\"id\" must be a whole number from 0 to 9007199254740992 (the sender's number for the "
                              "command, written back in its command event), such as {\"command\":\"pause\",\"id\":1}; "
                              "leave it out if no answer is looked for");
    }
    // A command refused for something else keeps its number: the sender finds that answer too.
    Command c = *parse_command(R"({"command":"jump","id":7})");
    CHECK(!c.accepted && c.id == std::optional<std::int64_t>(7));
    c = *parse_command(R"({"command":"step","id":8})");
    CHECK(!c.accepted && c.id == std::optional<std::int64_t>(8));

    // The event: "reason" after "message" of a refusal, "id" last; the time commands keep both.
    Sink sink;
    EventStream& e = sink.start();
    TimeCommands commands(false, [&e](const Command& done, std::string_view source) { e.command(done, source); });
    commands.set_time_lock({});
    static_cast<void>(commands.apply(*parse_command(R"({"command":"pause","id":41})"), "stdin"));
    static_cast<void>(commands.apply(*parse_command(R"({"command":"step","ms":10,"id":42})"), "stdin"));
    e.command(*parse_command(R"({"command":"jump","id":43})"), "stdin");
    e.command(*parse_command("x"), "stdin");
    CHECK(e.wait_written(10s));
    check_text(shape(sink.lines()), R"(command "command":"pause","source":"stdin","accepted":true,"message":null,"reason":null,"id":41}
command "command":"step","source":"stdin","accepted":false,"message":"the simulation has not been set up yet: step works once the simulation stands still","reason":"not-set-up","id":42}
command "command":"jump","source":"stdin","accepted":false,"message":"unknown command 'jump': the commands are stop, pause, resume, step, run_until and speed","reason":"unknown-command","id":43}
command "command":null,"source":"stdin","accepted":false,"message":"the line is not a JSON object: write one JSON object per line, such as {\"command\":\"stop\"}","reason":"invalid-command","id":null}
)");
}

TEST_CASE(the_time_commands_are_read_with_their_values)
{
    const auto read = [](std::string_view line) {
        const auto c = parse_command(line);
        return std::string(c->accepted ? "accepted " : "refused ") + (c->name ? *c->name : "-") +
               (c->ms ? " ms " + std::to_string(*c->ms) : "") + (c->at_ms ? " at_ms " + std::to_string(*c->at_ms) : "") + (c->speed ? " value " + speed_text(c->speed) : "") + ": " +
               c->message;
    };
    check_text(read(R"({"command":"pause"})"), "accepted pause: ");
    check_text(read(R"({"command":"resume","ms":5})"), "accepted resume: "); // other keys are ignored
    check_text(read(R"({"command":"step","ms":100})"), "accepted step ms 100: ");
    check_text(read(R"({"command":"step","ms":1000000000000})"), "accepted step ms 1000000000000: ");
    check_text(read(R"({"command":"run_until","at_ms":5000})"), "accepted run_until at_ms 5000: ");
    check_text(read(R"({"command":"run_until","at_ms":0})"), "accepted run_until at_ms 0: ");
    const std::string until = "refused run_until: run_until needs \"at_ms\", the virtual time to stand still at, a whole "
                              "number of milliseconds from 0 to 1000000000000, such as "
                              "{\"command\":\"run_until\",\"at_ms\":5000}";
    for (const char* bad : {R"({"command":"run_until"})", R"({"command":"run_until","at_ms":-1})",
                            R"({"command":"run_until","at_ms":1.5})", R"({"command":"run_until","at_ms":"5"})",
                            R"({"command":"run_until","at_ms":1000000000001})"})
    {
        check_text(read(bad), until);
        check_text(parse_command(bad)->reason, "invalid-argument");
    }
    check_text(read(R"({"command":"speed","value":1})"), "accepted speed value 1: ");
    check_text(read(R"({"command":"speed","value":0.25})"), "accepted speed value 0.25: ");
    check_text(read(R"({"command":"speed","value":"max"})"), "accepted speed: ");
    CHECK(parse_command(R"({"command":"speed","value":"max"})")->is_time());
    CHECK(!parse_command(R"({"command":"stop"})")->is_time());
    const std::string step = "refused step: step needs \"ms\", a whole number of milliseconds of virtual time from 1 to "
                             "1000000000000, such as {\"command\":\"step\",\"ms\":100}";
    for (const char* bad : {R"({"command":"step"})", R"({"command":"step","ms":0})", R"({"command":"step","ms":-5})",
                            R"({"command":"step","ms":1.5})", R"({"command":"step","ms":"100"})",
                            R"({"command":"step","ms":1000000000001})", R"({"command":"step","ms":[1]})"})
    {
        check_text(read(bad), step);
    }
    const std::string speed = "refused speed: speed needs \"value\", a number of simulation seconds per wall-clock second "
                              "from 0.000001 to 1000000 or \"max\", such as {\"command\":\"speed\",\"value\":1}";
    for (const char* bad : {R"({"command":"speed"})", R"({"command":"speed","value":0})", R"({"command":"speed","value":"1"})",
                            R"({"command":"speed","value":max})", R"({"command":"speed","value":-1})",
                            R"({"command":"speed","value":"fast"})", R"({"command":"speed","value":null})"})
    {
        check_text(read(bad), speed);
    }
}

TEST_CASE(time_commands_before_the_simulation_are_kept_and_after_it_refused)
{
    Sink sink;
    EventStream& e = sink.start(Sink::quiet());
    TimeCommands commands(false, [&e](const Command& command, std::string_view source) { e.command(command, source); });
    commands.set_time_lock({});
    const auto command = [](const std::string& line) { return *parse_command(line); };
    // Before the run has its controller: pause, resume and speed are kept; a step cannot be told
    // how far it goes yet.
    CHECK(commands.apply(command(R"({"command":"speed","value":2})"), "stdin").accepted);
    CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
    const Command early = commands.apply(command(R"({"command":"step","ms":10})"), "stdin");
    CHECK(!early.accepted);
    check_text(early.message, "the simulation has not been set up yet: step works once the simulation stands still");
    CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
    commands.detach();
    const Command late = commands.apply(command(R"({"command":"pause"})"), "stdin");
    CHECK(!late.accepted);
    check_text(late.message, "the simulation is not running (it is over, or it did not start): pause works from the start "
                             "of the run until the simulation ends");
    CHECK(e.wait_written(10s));
    check_text(shape(sink.lines()),
               R"(command "command":"speed","source":"stdin","accepted":true,"message":null,"reason":null,"id":null}
command "command":"pause","source":"stdin","accepted":true,"message":null,"reason":null,"id":null}
command "command":"step","source":"stdin","accepted":false,"message":"the simulation has not been set up yet: step works once the simulation stands still","reason":"not-set-up","id":null}
command "command":"resume","source":"stdin","accepted":true,"message":null,"reason":null,"id":null}
command "command":"pause","source":"stdin","accepted":false,"message":"the simulation is not running (it is over, or it did not start): pause works from the start of the run until the simulation ends","reason":"not-running","id":null}
)");
    // Without events: only the answer.
    TimeCommands quiet(true);
    quiet.set_time_lock({});
    CHECK(quiet.apply(command(R"({"command":"speed","value":"max"})"), "stdin").accepted);
}

TEST_CASE(time_commands_between_the_controller_and_the_start_take_effect_at_the_start)
{
    // The run has made its controller (attach()), the participants have not joined yet: a step
    // is accepted now (how far it goes is known) and waits in the gate for the start.
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    TimeCommands commands(true);
    commands.set_time_lock({});
    CHECK(commands.apply(command(R"({"command":"speed","value":0.5})"), "stdin").accepted);
    commands.attach(controller, 10, std::optional<double>(2.0));
    // The speed command before attach() wins over the configuration's speed; the run starts paused.
    TimeState s = controller.time_state();
    CHECK(s.speed == std::optional<double>(0.5));
    CHECK(s.pause_target == std::optional<SimTime>(SimTime{}));
    // Whole steps of the controller, counted from where it will stand: 25 ms are 30, and 5 more 40.
    Command step = commands.apply(command(R"({"command":"step","ms":25})"), "stdin");
    CHECK(step.accepted);
    CHECK(step.ms == std::optional<std::int64_t>(30));
    CHECK(step.at_ms == std::optional<std::int64_t>(30)); // where it will stand: said with the answer
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(30)));
    step = commands.apply(command(R"({"command":"step","ms":5})"), "stdin");
    CHECK(step.ms == std::optional<std::int64_t>(10));
    CHECK(step.at_ms == std::optional<std::int64_t>(40)); // from where the step before ends
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(40)));
    CHECK(commands.apply(command(R"({"command":"speed","value":"max"})"), "stdin").accepted);
    CHECK(!controller.time_state().speed);
    // Nothing ran: nothing is counted.
    s = controller.time_state();
    CHECK(s.paced_steps == 0 && s.running_for.count() == 0 && s.paused_for.count() == 0 && !s.paused);
    commands.detach();
    CHECK(!commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
}

TEST_CASE(an_accepted_step_says_where_it_ends_in_its_command_event)
{
    // The command event of a step has "at_ms": the virtual time at which the simulation stands
    // still after it, known when the step is accepted, and the gate is sent to that same time.
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    const auto target = [&controller] { return controller.time_state().pause_target; };
    const auto at = [](std::int64_t ms) { return std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(ms)); };
    Sink sink;
    EventStream& events = sink.start();
    // Not started paused: the gate goes on (here it waits for the start) until a pause.
    TimeCommands commands(false, [&events](const Command& c, std::string_view source) { events.command(c, source); });
    commands.set_time_lock({});
    commands.attach(controller, 10, std::nullopt);
    // Refused (not paused): no "at_ms".
    Command step = commands.apply(command(R"({"command":"step","ms":10,"id":1})"), "stdin");
    CHECK(!step.accepted && !step.at_ms);
    // A pause was accepted and the simulation has not paused yet: the step counts from the
    // controller's step in progress (0 here), and its event says where that is.
    CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
    CHECK(!controller.time_state().paused);
    step = commands.apply(command(R"({"command":"step","ms":15,"id":2})"), "stdin");
    CHECK(step.accepted && step.ms == std::optional<std::int64_t>(20) && step.at_ms == std::optional<std::int64_t>(20));
    CHECK(target() == at(20));
    // A second step while the first is in progress: from where the first ends.
    step = commands.apply(command(R"({"command":"step","ms":30,"id":3})"), "control_server");
    CHECK(step.at_ms == std::optional<std::int64_t>(50));
    CHECK(target() == at(50));
    // After a run_until: from the time of the run_until.
    CHECK(commands.apply(command(R"({"command":"run_until","at_ms":195})"), "stdin").at_ms == std::optional<std::int64_t>(200));
    step = commands.apply(command(R"({"command":"step","ms":10})"), "stdin");
    CHECK(step.at_ms == std::optional<std::int64_t>(210));
    CHECK(target() == at(210));
    // A resume ends it: the next step is refused and has no "at_ms".
    CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
    CHECK(!commands.apply(command(R"({"command":"step","ms":10})"), "stdin").at_ms);
    commands.detach();
    CHECK(events.wait_written(10s));
    const std::string refused = R"x(,"accepted":false,"message":"step works while the simulation is paused: pause it first","reason":"not-paused")x";
    check_text(shape(sink.lines()), R"(command "command":"step","source":"stdin")" + refused + R"(,"id":1}
command "command":"pause","source":"stdin","accepted":true,"message":null,"reason":null,"id":null}
command "command":"step","source":"stdin","accepted":true,"message":null,"reason":null,"ms":20,"at_ms":20,"id":2}
command "command":"step","source":"control_server","accepted":true,"message":null,"reason":null,"ms":30,"at_ms":50,"id":3}
command "command":"run_until","source":"stdin","accepted":true,"message":null,"reason":null,"at_ms":200,"id":null}
command "command":"step","source":"stdin","accepted":true,"message":null,"reason":null,"ms":10,"at_ms":210,"id":null}
command "command":"resume","source":"stdin","accepted":true,"message":null,"reason":null,"id":null}
command "command":"step","source":"stdin")" + refused + R"(,"id":null}
)");
}

TEST_CASE(a_step_after_a_pause_counts_from_the_pause_and_not_from_the_run_until_it_ended)
{
    // A run_until (or a step) is in progress, a pause is accepted, and a step comes before the
    // simulation has paused: the pause has ended the run_until, so the step counts from the
    // controller's step in progress (0 here: the gate waits for the start, no step is held).
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    const auto target = [&controller] { return controller.time_state().pause_target; };
    const auto at = [](std::int64_t ms) { return std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(ms)); };
    TimeCommands commands(false);
    commands.set_time_lock({});
    commands.attach(controller, 10, std::nullopt);
    CHECK(commands.apply(command(R"({"command":"run_until","at_ms":100000})"), "stdin").accepted);
    CHECK(target() == at(100000));
    CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
    CHECK(!controller.time_state().paused);
    CHECK(!target());
    Command step = commands.apply(command(R"({"command":"step","ms":10})"), "stdin");
    CHECK(step.accepted && step.at_ms == std::optional<std::int64_t>(10));
    CHECK(target() == at(10));
    // The same after a step in progress: the pause ends it, and the next step starts anew.
    CHECK(commands.apply(command(R"({"command":"pause"})"), "control_server").accepted);
    CHECK(!target());
    step = commands.apply(command(R"({"command":"step","ms":30})"), "control_server");
    CHECK(step.at_ms == std::optional<std::int64_t>(30));
    CHECK(target() == at(30));
    commands.detach();
}

TEST_CASE(a_step_after_a_pause_counts_from_where_the_simulation_stands_and_not_from_a_pause_time_ahead)
{
    // A simulation that runs (the controller and one participant) and stands still at 50 ms. A pause with a
    // time ahead leaves it standing there; a pause gives that time up; the step then counts from
    // 50 ms (not from 0, and not from the time ahead), and the simulation arrives where it said.
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"Peer"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10), .start_paused = true};
    SimulationController controller(options);
    ParticipantRuntime peer(vehicle_sil::unit::coordinated_options("Peer", registry.uri, VirtualTimeOptions{10ms, std::nullopt}));
    peer.set_step_handler([](SimTime, SimDuration) {});
    const auto command = [](const std::string& line) { return *parse_command(line); };
    const auto at = [](std::int64_t ms) { return std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(ms)); };
    TimeCommands commands(true);
    commands.set_time_lock({});
    commands.attach(controller, 10, std::nullopt);
    vehicle_sil::unit::start_together(controller, {&peer}, vehicle_sil::test::scaled_timeout(30s));
    CHECK(commands.apply(command(R"({"command":"run_until","at_ms":50})"), "stdin").accepted);
    CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)) == at(50));
    Command ahead;
    ahead.name = "pause";
    ahead.accepted = true; // as the control server hands it over
    ahead.at_ms = 100000;
    CHECK(commands.apply(ahead, "control_server").accepted);
    CHECK(controller.time_state().paused);
    CHECK(controller.time_state().pause_target == at(100000));
    CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
    CHECK(!controller.time_state().pause_target);
    const Command step = commands.apply(command(R"({"command":"step","ms":10})"), "stdin");
    CHECK(step.accepted && step.at_ms == std::optional<std::int64_t>(60));
    CHECK(controller.wait_until_paused(vehicle_sil::test::scaled_timeout(10s)) == at(60));
    commands.detach();
    controller.stop("test done");
    peer.stop("test done");
}

TEST_CASE(time_commands_from_another_thread_never_reach_a_controller_that_is_gone)
{
    // The thread that reads the commands applies them while the run attaches its controller,
    // detaches it and destroys it (under ThreadSanitizer and AddressSanitizer this is the test).
    const auto command = [](const std::string& line) { return *parse_command(line); };
    const std::vector<Command> lines{command(R"({"command":"pause"})"), command(R"({"command":"step","ms":10})"),
                                     command(R"({"command":"speed","value":2})"), command(R"({"command":"resume"})"),
                                     command(R"({"command":"speed","value":"max"})")};
    for (int round = 0; round < 3; ++round)
    {
        vehicle_sil::unit::InProcessRegistry registry;
        TimeCommands commands(round % 2 == 0);
        commands.set_time_lock({});
        std::atomic<bool> done{false};
        std::atomic<int> refused_after_detach{0};
        std::atomic<bool> detached{false};
        std::mutex sync_mutex;
        std::condition_variable sync_cv;
        std::atomic<int> calls{0};
        std::atomic<int> notify_at{0};
        const auto wait_calls = [&](int n) {
            const int target = calls.load(std::memory_order_acquire) + n;
            std::unique_lock lock(sync_mutex);
            notify_at.store(target, std::memory_order_release);
            CHECK(sync_cv.wait_for(lock, vehicle_sil::test::scaled_timeout(10s), [&] {
                return calls.load(std::memory_order_acquire) >= target;
            }));
        };
        std::thread reader([&] {
            std::size_t i = 0;
            while (!done.load())
            {
                const bool after = detached.load();
                const Command out = commands.apply(lines[i++ % lines.size()], "stdin");
                const int c = calls.fetch_add(1, std::memory_order_acq_rel) + 1;
                if (const int target = notify_at.load(std::memory_order_acquire); target > 0 && c >= target)
                {
                    const std::lock_guard lock(sync_mutex);
                    notify_at.store(0, std::memory_order_relaxed);
                    sync_cv.notify_all();
                }
                if (after && !out.accepted)
                {
                    ++refused_after_detach;
                }
                else if (after)
                {
                    refused_after_detach = -1000000; // accepted after the end
                }
            }
        });
        {
            SimulationControllerOptions options;
            options.registry_uri = registry.uri;
            options.required_participants = {"NeverJoins"};
            options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
            SimulationController controller(options);
            wait_calls(20);
            commands.attach(controller, 10, std::nullopt);
            wait_calls(20);
            commands.detach();
            detached = true;
        } // the controller goes while the reader still applies commands
        wait_calls(20);
        done = true;
        reader.join();
        CHECK(refused_after_detach.load() > 0);
    }
}

TEST_CASE(time_commands_of_a_control_server_run_until_and_pause_at_a_time_and_are_checked)
{
    // What a control server sends (run_until, pause with at_ms, numbers that no line of standard
    // input could carry) goes the same way, with the same event.
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    Sink sink;
    EventStream& e = sink.start(Sink::quiet());
    TimeCommands commands(false, [&e](const Command& command, std::string_view source) { e.command(command, source); });
    commands.set_time_lock({});
    const auto command = [](const char* name) {
        Command c;
        c.name = name;
        c.accepted = true;
        return c;
    };
    Command until = command("run_until");
    until.at_ms = 25;
    CHECK(!commands.apply(until, "control_server").accepted); // no controller yet
    commands.attach(controller, 10, std::nullopt);
    until = commands.apply(until, "control_server");
    CHECK(until.accepted && until.at_ms == std::optional<std::int64_t>(30));
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(30)));
    // It will stand still: a step is a step while paused, from there.
    Command step = command("step");
    step.ms = 1;
    CHECK(commands.apply(step, "control_server").accepted);
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(40)));
    Command at = command("pause");
    at.at_ms = 101;
    CHECK(commands.apply(at, "control_server").at_ms == std::optional<std::int64_t>(110));
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(110)));
    Command speed = command("speed");
    speed.speed = 0.0;
    check_text(commands.apply(speed, "control_server").message,
               "the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max");
    step.ms = 0;
    check_text(commands.apply(step, "control_server").message, "step needs a time from 1 to 1000000000000 ms");
    at.at_ms = -1;
    check_text(commands.apply(at, "control_server").message, "pause at a time needs a time from 0 to 1000000000000 ms");
    commands.detach();
    CHECK(e.wait_written(10s));
    check_text(shape(sink.lines()),
               R"(command "command":"run_until","source":"control_server","accepted":false,"message":"the simulation has not been set up yet: run_until at a time works once the participants have been started","reason":"not-set-up","id":null}
command "command":"run_until","source":"control_server","accepted":true,"message":null,"reason":null,"at_ms":30,"id":null}
command "command":"step","source":"control_server","accepted":true,"message":null,"reason":null,"ms":10,"at_ms":40,"id":null}
command "command":"pause","source":"control_server","accepted":true,"message":null,"reason":null,"at_ms":110,"id":null}
command "command":"speed","source":"control_server","accepted":false,"message":"the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max","reason":"out-of-range","id":null}
command "command":"step","source":"control_server","accepted":false,"message":"step needs a time from 1 to 1000000000000 ms","reason":"out-of-range","id":null}
command "command":"pause","source":"control_server","accepted":false,"message":"pause at a time needs a time from 0 to 1000000000000 ms","reason":"out-of-range","id":null}
)");
}

TEST_CASE(time_commands_note_the_first_that_changed_how_the_time_goes)
{
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    // Starting paused, a resume, a speed of 1, a stop and a refused command change nothing.
    TimeCommands commands(true);
    commands.set_time_lock({});
    CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
    CHECK(commands.apply(command(R"({"command":"speed","value":1})"), "stdin").accepted);
    CHECK(!commands.apply(command(R"({"command":"step","ms":10})"), "stdin").accepted);
    CHECK(commands.apply(command(R"({"command":"stop"})"), "stdin").accepted);
    commands.attach(controller, 10, std::nullopt);
    CHECK(!commands.apply(command(R"({"command":"step","ms":10})"), "stdin").accepted); // not paused
    CHECK(!commands.first_time_change());
    // The first that does is kept, with where it was to stand still.
    Command at;
    at.name = "pause";
    at.accepted = true;
    at.at_ms = 25;
    CHECK(commands.apply(at, "control_server").accepted);
    CHECK(commands.apply(command(R"({"command":"speed","value":2})"), "stdin").accepted);
    commands.detach();
    const std::optional<TimeChange> first = commands.first_time_change();
    CHECK(first && first->command == "pause" && first->source == "control_server" && first->sim_ms == 0);
    CHECK(first->at_ms == std::optional<std::int64_t>(30) && !first->ms && !first->speed);
    // A step, with how far it goes in whole steps and where it was to stand after it.
    TimeCommands stepped(true);
    stepped.set_time_lock({});
    stepped.attach(controller, 10, std::nullopt);
    CHECK(stepped.apply(command(R"({"command":"step","ms":25})"), "stdin").accepted);
    stepped.detach();
    const std::optional<TimeChange> step = stepped.first_time_change();
    CHECK(step && step->command == "step" && step->ms == std::optional<std::int64_t>(30) &&
          step->at_ms == std::optional<std::int64_t>(30));
    // A speed before the controller: max, and any other than 1.
    TimeCommands fastest(false);
    fastest.set_time_lock({});
    CHECK(fastest.apply(command(R"({"command":"speed","value":"max"})"), "stdin").accepted);
    const std::optional<TimeChange> max = fastest.first_time_change();
    CHECK(max && max->command == "speed" && !max->speed && max->sim_ms == 0 && max->source == "stdin");
    TimeCommands slower(false);
    slower.set_time_lock({});
    CHECK(slower.apply(command(R"({"command":"speed","value":0.5})"), "stdin").accepted);
    CHECK(slower.first_time_change() && slower.first_time_change()->speed == std::optional<double>(0.5));
}

TEST_CASE(the_hold_before_the_start_ends_with_resume_step_or_run_until_a_give_up_or_the_detach)
{
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    using Hold = TimeCommands::Hold;
    for (const char* release : {R"({"command":"resume"})", R"({"command":"step","ms":10})", "run_until", "pause_at"})
    {
        TimeCommands commands(true);
        commands.set_time_lock({});
        commands.attach(controller, 10, std::nullopt);
        std::atomic<int> result{-1};
        std::atomic<int> began{0};
        std::thread window([&] {
            result = static_cast<int>(commands.hold_before_start([] { return false; }, std::chrono::milliseconds(5), [&] { ++began; }));
        });
        // In the hold: it stands still at 0, and neither a pause nor a speed lets it go.
        CHECK(commands.wait_paused(std::chrono::seconds(10)) == std::optional<SimTime>(SimTime{}));
        bool held = false;
        CHECK(commands.inspect([&held](SimulationController&, bool holding) { held = holding; }) == TimeCommands::Stage::Attached);
        CHECK(held);
        CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
        CHECK(commands.apply(command(R"({"command":"speed","value":2})"), "stdin").accepted);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        CHECK(result.load() == -1);
        Command go;
        if (std::string(release) == "run_until" || std::string(release) == "pause_at")
        {
            go.name = std::string(release) == "run_until" ? "run_until" : "pause";
            go.accepted = true;
            go.at_ms = 50;
        }
        else
        {
            go = command(release);
        }
        CHECK(commands.apply(go, "control_server").accepted);
        window.join();
        CHECK(result.load() == static_cast<int>(Hold::Released));
        CHECK(began.load() == 1);
        commands.detach();
    }
    // A give-up ends it, and so does the end of the run.
    TimeCommands commands(true);
    commands.set_time_lock({});
    commands.attach(controller, 10, std::nullopt);
    std::atomic<bool> give_up{false};
    std::atomic<int> result{-1};
    std::thread window([&] { result = static_cast<int>(commands.hold_before_start([&] { return give_up.load(); }, std::chrono::milliseconds(5))); });
    give_up = true;
    window.join();
    CHECK(result.load() == static_cast<int>(Hold::GaveUp));
    bool held = true;
    static_cast<void>(commands.inspect([&held](SimulationController&, bool holding) { held = holding; }));
    CHECK(!held); // not left holding
    std::thread again([&] { result = static_cast<int>(commands.hold_before_start([] { return false; }, std::chrono::milliseconds(5))); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    int detached = 0;
    commands.set_on_detach([&detached](SimulationController&) { ++detached; });
    commands.detach();
    again.join();
    CHECK(result.load() == static_cast<int>(Hold::GaveUp));
    commands.detach();
    CHECK(detached == 1); // the last look, once, while the controller is there
    CHECK(!commands.wait_paused(std::chrono::seconds(10)));
    CHECK(commands.inspect([](SimulationController&, bool) {}) == TimeCommands::Stage::Detached);
    CHECK(commands.hold_before_start([] { return false; }, std::chrono::milliseconds(5)) == Hold::GaveUp);
    // A stop of a control server is refused once the run is over (its result stays).
    Command stop;
    stop.name = "stop";
    stop.accepted = true;
    CHECK(!commands.apply(stop, "control_server").accepted);
}

TEST_CASE(the_commands_before_the_window_decide_whether_the_run_holds)
{
    // Started paused, the run holds only if, when the window opens, it is to stand still at 0:
    // a resume, step or run_until (or pause at a later time) before it lets it go on without a
    // hold; a pause after them asks for the hold again. The same for every source.
    vehicle_sil::unit::InProcessRegistry registry;
    SimulationControllerOptions options;
    options.registry_uri = registry.uri;
    options.required_participants = {"NeverJoins"};
    options.time_control = TimeControlOptions{.step = std::chrono::milliseconds(10)};
    SimulationController controller(options);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    using Hold = TimeCommands::Hold;
    const auto hold = [](TimeCommands& commands) {
        // Gives up at once if it holds (the window would wait for a command).
        return commands.hold_before_start([] { return true; }, std::chrono::milliseconds(1));
    };
    {
        // A resume before attach() and one after it.
        TimeCommands before(true);
        before.set_time_lock({});
        CHECK(before.apply(command(R"({"command":"resume"})"), "control_server").accepted);
        before.attach(controller, 10, std::nullopt);
        CHECK(hold(before) == Hold::None);
        CHECK(!controller.time_state().pause_target); // it goes on from 0
        before.detach();
        TimeCommands after(true);
        after.set_time_lock({});
        after.attach(controller, 10, std::nullopt);
        CHECK(after.apply(command(R"({"command":"resume"})"), "stdin").accepted);
        CHECK(hold(after) == Hold::None);
        after.detach();
    }
    {
        // A step before the window: done from 0 (it stands still at 20), without a hold.
        TimeCommands commands(true);
        commands.set_time_lock({});
        commands.attach(controller, 10, std::nullopt);
        CHECK(commands.apply(command(R"({"command":"step","ms":15})"), "stdin").accepted);
        CHECK(hold(commands) == Hold::None);
        CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(20)));
        commands.detach();
    }
    {
        // A resume and then a pause: it holds again.
        TimeCommands commands(true);
        commands.set_time_lock({});
        commands.attach(controller, 10, std::nullopt);
        CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
        CHECK(commands.apply(command(R"({"command":"pause"})"), "control_server").accepted);
        CHECK(hold(commands) == Hold::GaveUp);
        commands.detach();
    }
    {
        // Not started paused, a pause before the window: it holds.
        TimeCommands commands(false);
        commands.set_time_lock({});
        CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
        commands.attach(controller, 10, std::nullopt);
        CHECK(hold(commands) == Hold::GaveUp);
        commands.detach();
    }
    {
        // Not started paused, nothing asked: no hold.
        TimeCommands commands(false);
        commands.set_time_lock({});
        commands.attach(controller, 10, std::nullopt);
        CHECK(hold(commands) == Hold::None);
        commands.detach();
    }
}

TEST_CASE(the_input_is_cut_into_lines_and_an_overlong_line_is_one_refused_command)
{
    std::vector<Command> got;
    CommandInput input([&got](const Command& c) { got.push_back(c); });
    input.append("{\"comm");
    input.append("and\":\"stop\"}\n\nnot json\r\n{\"command\":");
    CHECK(got.size() == 2);
    input.append("\"stop\"}");
    CHECK(got.size() == 2); // no line end yet
    input.finish();
    CHECK(got.size() == 3);
    CHECK(got[0].is_stop() && !got[1].accepted && got[2].is_stop());

    got.clear();
    CommandInput longer([&got](const Command& c) { got.push_back(c); });
    const std::string chunk(4096, 'x');
    for (std::size_t n = 0; n <= kMaxCommandLine / chunk.size(); ++n)
    {
        longer.append(chunk);
    }
    CHECK(got.size() == 1 && !got[0].accepted);
    check_text(got[0].message, "the line is longer than 65536 bytes");
    longer.append(chunk + "\n{\"command\":\"stop\"}\n");
    longer.finish();
    CHECK(got.size() == 2 && got[1].is_stop());
}

TEST_CASE(version_names_the_formats_and_the_features)
{
    const std::string v = version();
    check_text(format_version_text(), "vehicle_sil_launch " + v + "\n");
    check_text(format_version_json(), R"({
  "vehicle_sil_launch": 1,
  "vehicle_sil": ")" + v + R"(",
  "formats": {
    "check": 1,
    "run": 2,
    "events": 2,
    "describe": 1,
    "test": 1,
    "timeline": 1,
    "connections": 1
  },
  "features": [
    "events",
    "stdin-commands",
    "check-variables",
    "speed",
    "start-paused",
    "time-commands",
    "control-links",
    "scenario",
    "manual-participants",
    "scenario-wave",
    "scenario-motion-expect",
    "scenario-points-file",
    "timeline",
    "timeline-lines",
    "scenario-inputs",
    "network-names",
    "run-traffic",
    "j1939-nodes",
    "connect",
    "connections",
    "connect-counts",
    "state",
    "time-policy",
    "stdin-run-until",
    "request",
    "scenario-samples",
    "run-stop-at",
    "serve",
    "check-file-only",
    "serve-prepare"
  ]
}
)");
    // A build with the control server adds its features last.
    const std::string with = format_version_json({kControlServerFeature, kScenarioControlFeature, kScenarioTimeFeature});
    CHECK(with.find("    \"connect-counts\",\n    \"state\",\n    \"time-policy\",\n    \"stdin-run-until\",\n    \"request\",\n    \"scenario-samples\",\n    \"run-stop-at\",\n    \"serve\",\n    \"check-file-only\",\n    \"serve-prepare\",\n    \"control-server\",\n    \"scenario-control\",\n    \"scenario-time\"\n  ]") != std::string::npos);
}

TEST_CASE(event_stream_formats_prepare_and_status_replies)
{
    std::string written;
    std::mutex mu;
    std::condition_variable cv;
    auto count_lines = [&] {
        return static_cast<std::size_t>(std::count(written.begin(), written.end(), '\n'));
    };
    EventStreamOptions opts;
    opts.heartbeat = 0ms;
    opts.now_ms = [n = std::make_shared<std::atomic<std::int64_t>>(0)]() { return n->fetch_add(1); };
    {
        EventStream stream(
            [&](std::string_view bytes) {
                EventWrite w;
                const std::lock_guard lock(mu);
                written.append(bytes);
                cv.notify_all();
                w.bytes = bytes.size();
                return w;
            },
            [](const std::string&) {}, opts);

        PrepareResult prep_ok;
        prep_ok.accepted = true;
        prep_ok.status.instance = "0123456789abcdef";
        prep_ok.status.status_rev = 2;
        prep_ok.status.launcher_state = LauncherState::Ready;
        prep_ok.status.config = "sim.yaml";
        prep_ok.status.eligibility = HotEligibility{true, "", ""};
        prep_ok.metadata.participant_names = {"Engine", "Dash"};
        prep_ok.metadata.network_names = {"CAN1"};
        prep_ok.metadata.variants = {"fast"};
        prep_ok.metadata.file_sha256 = std::string(64, 'a');
        stream.prepare(prep_ok, 11);

        PrepareResult prep_busy;
        prep_busy.accepted = false;
        prep_busy.reason = serve_reason::kBusy;
        prep_busy.message = "busy";
        prep_busy.status = prep_ok.status;
        prep_busy.status.launcher_state = LauncherState::Running;
        prep_busy.status.active_run_id = "0123456789abcdef-1";
        prep_busy.status.run_state.emplace();
        prep_busy.status.run_state->rev = 3;
        prep_busy.status.run_state->phase = runner::Phase::Running;
        stream.prepare(prep_busy, std::nullopt);

        stream.status(prep_busy.status, 12);

        std::unique_lock lock(mu);
        CHECK(cv.wait_for(lock, 5000ms, [&] { return count_lines() >= 3; }));
    }
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < written.size())
    {
        const auto nl = written.find('\n', pos);
        lines.push_back(written.substr(pos, nl - pos));
        pos = nl + 1;
    }
    CHECK_EQ(lines.size(), std::size_t{3});
    CHECK(lines[0].find(R"("event":"prepare")") != std::string::npos);
    CHECK(lines[0].find(R"("id":11)") != std::string::npos);
    CHECK(lines[0].find(R"("accepted":true)") != std::string::npos);
    CHECK(lines[0].find(R"("participants":["Engine","Dash"])") != std::string::npos);
    CHECK(lines[0].find(R"("networks":["CAN1"])") != std::string::npos);
    CHECK(lines[0].find(R"("variants":["fast"])") != std::string::npos);
    CHECK(lines[0].find(std::string(64, 'a')) != std::string::npos);
    CHECK(lines[0].find(R"("run":")") == std::string::npos);

    CHECK(lines[1].find(R"("event":"prepare")") != std::string::npos);
    CHECK(lines[1].find(R"("id":null)") != std::string::npos);
    CHECK(lines[1].find(R"("accepted":false)") != std::string::npos);
    CHECK(lines[1].find(R"("reason":"busy")") != std::string::npos);
    CHECK(lines[1].find(R"("metadata":null)") != std::string::npos);
    CHECK(lines[1].find(R"("findings":[])") != std::string::npos);

    CHECK(lines[2].find(R"("event":"status")") != std::string::npos);
    CHECK(lines[2].find(R"("id":12)") != std::string::npos);
    CHECK(lines[2].find(R"("launcher_state":"running")") != std::string::npos);
    CHECK(lines[2].find(R"("active_run_id":"0123456789abcdef-1")") != std::string::npos);
    CHECK(lines[2].find(R"("run_state":{"rev":3,"phase":"running")") != std::string::npos);
}

TEST_CASE(an_accepted_stop_is_asked_for_before_the_detach_and_a_refused_one_not_at_all)
{
    TimeCommands commands(false);
    commands.set_time_lock({});
    Command stop;
    stop.name = "stop";
    stop.accepted = true;
    std::atomic<bool> detached{false};
    std::thread detacher;
    int asked = 0;
    // The run detaches the commands while the stop is being asked for: it waits until it is.
    const Command done = commands.apply(stop, "control_server", [&] {
        ++asked;
        std::promise<void> started;
        auto startedFuture = started.get_future();
        detacher = std::thread([&] {
            started.set_value();
            commands.detach();
            detached = true;
        });
        CHECK(startedFuture.wait_for(vehicle_sil::test::scaled_timeout(5s)) == std::future_status::ready);
        std::this_thread::sleep_for(10ms);
        CHECK(!detached.load());
    });
    detacher.join();
    CHECK(done.accepted && asked == 1 && detached.load());
    // Once detached, a stop is refused and not asked for.
    const Command late = commands.apply(stop, "control_server", [&] { ++asked; });
    CHECK(!late.accepted && asked == 1);
}

TEST_CASE(stdin_command_rejects_integers_that_overflow_before_range_checks)
{
    for (const char* line : {R"({"command":"stop","id":18446744073709551616})",
                             R"({"command":"run_until","at_ms":18446744073709551616})",
                             R"({"command":"step","ms":18446744073709551616})"})
    {
        const auto parsed = parse_command(line);
        CHECK(parsed.has_value());
        CHECK(!parsed->accepted);
        CHECK(parsed->reason == "invalid-argument");
    }
}
