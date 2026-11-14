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
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using vehicle_sil::launch::test::check_text;

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
    e.time(TimeEvent{.sim_ms = 1010, .speed = 0.5, .paused = true, .actual_speed = 0.48, .late_steps = 3, .max_late_ms = 12,
                     .slipped_ms = 30, .late_ms = 4});
    // A participant's line may hold anything: quotes, a backslash, a tab, a line end.
    e.log("Engine", "say \"hi\"\\\tthen\nmore");
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
               R"({"event":"hello","seq":0,"t_ms":7,"vehicle_sil_events":1,"vehicle_sil":")" + v +
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
{"event":"time","seq":10,"t_ms":7,"sim_ms":1000,"speed":"max","paused":false,"actual_speed":null,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
{"event":"time","seq":11,"t_ms":7,"sim_ms":1010,"speed":0.5,"paused":true,"actual_speed":0.48,"late_steps":3,"max_late_ms":12,"slipped_ms":30,"late_ms":4}
{"event":"log","seq":12,"t_ms":7,"source":"Engine","line":"say \"hi\"\\\tthen\nmore"}
{"event":"participant","seq":13,"t_ms":7,"name":"Engine","state":"ended","exit_code":0,"signal":null}
{"event":"participant","seq":14,"t_ms":7,"name":"Crash","state":"ended","exit_code":null,"signal":")" +
                   process::signal_name(11) + R"("}
{"event":"participant","seq":15,"t_ms":7,"name":"Deaf","state":"stop_requested"}
{"event":"participant","seq":16,"t_ms":7,"name":"Deaf","state":"killed"}
{"event":"finding","seq":17,"t_ms":7,"severity":"note","code":"interrupted","file":"sim.yaml","line":null,"participant":null,"network":null,"message":"interrupted (stop command): ...","detail":null}
{"event":"finding","seq":18,"t_ms":7,"severity":"error","code":"exit-code","file":"sim.yaml","line":5,"participant":"A","network":null,"message":"participant 'A': ...","detail":"more"}
{"event":"command","seq":19,"t_ms":7,"command":null,"source":"stdin","accepted":false,"message":"the line is not a JSON object"}
{"event":"command","seq":20,"t_ms":7,"command":"stop","source":"eof","accepted":true,"message":null}
{"event":"phase","seq":21,"t_ms":7,"phase":"ended"}
)");
    CHECK(sink.closed().empty() && !e.closed());
}

TEST_CASE(the_check_and_the_result_are_their_json_on_one_line_and_nothing_follows_the_result)
{
    Sink sink;
    EventStreamOptions options = Sink::quiet();
    options.heartbeat = 10ms; // nor a heartbeat
    EventStream& e = sink.start(options);
    CheckReport check;
    check.file = "x.yaml";
    check.findings.push_back(Finding{Severity::Error, "file", "the file does not exist: check the path", 0, "", ""});
    e.check(check);
    RunReport report;
    report.check = check;
    // The result is written when result() returns.
    CHECK(e.result(report));
    CHECK(sink.lines().size() >= 2 && sink.lines().back().find("\"event\":\"result\"") != std::string::npos);
    const std::size_t count = sink.lines().size();
    e.log("late", "after the result");
    e.phase(Phase::Ended);
    CHECK(!e.result(report)); // only one result
    std::this_thread::sleep_for(100ms);
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
    CHECK(!e.result(RunReport{}));
    std::this_thread::sleep_for(50ms);
    CHECK(sink.closed().size() == 1);
    check_text(shape(sink.lines()), "phase \"phase\":\"running\"}\n");
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
    CHECK(e.result(RunReport{}));
    std::string all = shape(sink.lines());
    all.erase(all.rfind("result ")); // the result's JSON: another test
    check_text(all, R"(phase "phase":"running"}
log "source":"A","line":"1"}
log "source":"B","line":"2"}
log "source":"A","line":"3"}
log_dropped "count":4,"sources":{"A":3,"B":1}}
participant "name":"A","state":"joined"}
time "sim_ms":5,"speed":"max","paused":false,"actual_speed":null,"late_steps":0,"max_late_ms":0,"slipped_ms":0,"late_ms":0}
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
    std::this_thread::sleep_for(50ms);
    // The write that was waiting may end; nothing follows it.
    CHECK(sink.text().empty() || sink.text() == "{\"event\":\"phase\",\"seq\":0,\"t_ms\":7,\"phase\":\"running\"}\n");
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
        CHECK(!e.result(RunReport{}));
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
    CHECK(e.result(RunReport{}));
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
        const auto took = time_of([&] { written = e.result(RunReport{}); });
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
        const auto took = time_of([&] { written = e.result(RunReport{}); });
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
        const auto took = time_of([&] { written = e.result(RunReport{}, [&asked] { return ++asked >= 3; }); });
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
        CHECK(e.result(RunReport{}, [] { return true; }));
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
        std::thread reader([&sink] {
            std::this_thread::sleep_for(200ms);
            sink.unblock();
        });
        const bool written = e.result(RunReport{});
        reader.join();
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
               "refused 'jump': unknown command 'jump': the commands are stop, pause, resume, step and speed");
    CHECK(!parse_command(R"({"command":"jump"})")->is_stop());
}

TEST_CASE(the_time_commands_are_read_with_their_values)
{
    const auto read = [](std::string_view line) {
        const auto c = parse_command(line);
        return std::string(c->accepted ? "accepted " : "refused ") + (c->name ? *c->name : "-") +
               (c->ms ? " ms " + std::to_string(*c->ms) : "") + (c->speed ? " value " + speed_text(c->speed) : "") + ": " +
               c->message;
    };
    check_text(read(R"({"command":"pause"})"), "accepted pause: ");
    check_text(read(R"({"command":"resume","ms":5})"), "accepted resume: "); // other keys are ignored
    check_text(read(R"({"command":"step","ms":100})"), "accepted step ms 100: ");
    check_text(read(R"({"command":"step","ms":1000000000000})"), "accepted step ms 1000000000000: ");
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
    TimeCommands commands(false, &e);
    const auto command = [](const std::string& line) { return *parse_command(line); };
    // Before the run has its controller: pause, resume and speed are kept; a step cannot be told
    // how far it goes yet.
    CHECK(commands.apply(command(R"({"command":"speed","value":2})"), "stdin").accepted);
    CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
    const Command early = commands.apply(command(R"({"command":"step","ms":10})"), "stdin");
    CHECK(!early.accepted);
    check_text(early.message, "the simulation has not been set up yet: step works once it stands still (a time event "
                              "with paused true), with --start-paused from the start");
    CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
    commands.detach();
    const Command late = commands.apply(command(R"({"command":"pause"})"), "stdin");
    CHECK(!late.accepted);
    check_text(late.message, "the simulation is not running (it is over, or it did not start): pause works from the start "
                             "of the run until the simulation ends");
    CHECK(e.wait_written(10s));
    check_text(shape(sink.lines()),
               R"(command "command":"speed","source":"stdin","accepted":true,"message":null}
command "command":"pause","source":"stdin","accepted":true,"message":null}
command "command":"step","source":"stdin","accepted":false,"message":"the simulation has not been set up yet: step works once it stands still (a time event with paused true), with --start-paused from the start"}
command "command":"resume","source":"stdin","accepted":true,"message":null}
command "command":"pause","source":"stdin","accepted":false,"message":"the simulation is not running (it is over, or it did not start): pause works from the start of the run until the simulation ends"}
)");
    // Without events: only the answer.
    TimeCommands quiet(true);
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
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(30)));
    step = commands.apply(command(R"({"command":"step","ms":5})"), "stdin");
    CHECK(step.ms == std::optional<std::int64_t>(10));
    CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(40)));
    CHECK(commands.apply(command(R"({"command":"speed","value":"max"})"), "stdin").accepted);
    CHECK(!controller.time_state().speed);
    // Nothing ran: nothing is counted.
    s = controller.time_state();
    CHECK(s.paced_steps == 0 && s.running_for.count() == 0 && s.paused_for.count() == 0 && !s.paused);
    commands.detach();
    CHECK(!commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
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
        std::atomic<bool> done{false};
        std::atomic<int> refused_after_detach{0};
        std::atomic<bool> detached{false};
        std::thread reader([&] {
            std::size_t i = 0;
            while (!done.load())
            {
                const bool after = detached.load();
                const Command out = commands.apply(lines[i++ % lines.size()], "stdin");
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
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            commands.attach(controller, 10, std::nullopt);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            commands.detach();
            detached = true;
        } // the controller goes while the reader still applies commands
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
    TimeCommands commands(false, &e);
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
               R"(command "command":"run_until","source":"control_server","accepted":false,"message":"the simulation has not been set up yet: run_until at a time works once the participants have been started"}
command "command":"run_until","source":"control_server","accepted":true,"message":null,"at_ms":30}
command "command":"step","source":"control_server","accepted":true,"message":null,"ms":10}
command "command":"pause","source":"control_server","accepted":true,"message":null,"at_ms":110}
command "command":"speed","source":"control_server","accepted":false,"message":"the speed must be from 0.000001 to 1000000 simulation seconds per wall-clock second, or max"}
command "command":"step","source":"control_server","accepted":false,"message":"step needs a time from 1 to 1000000000000 ms"}
command "command":"pause","source":"control_server","accepted":false,"message":"pause at a time needs a time from 0 to 1000000000000 ms"}
)");
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
        CHECK(before.apply(command(R"({"command":"resume"})"), "control_server").accepted);
        before.attach(controller, 10, std::nullopt);
        CHECK(hold(before) == Hold::None);
        CHECK(!controller.time_state().pause_target); // it goes on from 0
        before.detach();
        TimeCommands after(true);
        after.attach(controller, 10, std::nullopt);
        CHECK(after.apply(command(R"({"command":"resume"})"), "stdin").accepted);
        CHECK(hold(after) == Hold::None);
        after.detach();
    }
    {
        // A step before the window: done from 0 (it stands still at 20), without a hold.
        TimeCommands commands(true);
        commands.attach(controller, 10, std::nullopt);
        CHECK(commands.apply(command(R"({"command":"step","ms":15})"), "stdin").accepted);
        CHECK(hold(commands) == Hold::None);
        CHECK(controller.time_state().pause_target == std::optional<SimTime>(SimTime{} + std::chrono::milliseconds(20)));
        commands.detach();
    }
    {
        // A resume and then a pause: it holds again.
        TimeCommands commands(true);
        commands.attach(controller, 10, std::nullopt);
        CHECK(commands.apply(command(R"({"command":"resume"})"), "stdin").accepted);
        CHECK(commands.apply(command(R"({"command":"pause"})"), "control_server").accepted);
        CHECK(hold(commands) == Hold::GaveUp);
        commands.detach();
    }
    {
        // Not started paused, a pause before the window: it holds.
        TimeCommands commands(false);
        CHECK(commands.apply(command(R"({"command":"pause"})"), "stdin").accepted);
        commands.attach(controller, 10, std::nullopt);
        CHECK(hold(commands) == Hold::GaveUp);
        commands.detach();
    }
    {
        // Not started paused, nothing asked: no hold.
        TimeCommands commands(false);
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
    "run": 1,
    "events": 1,
    "describe": 1
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
    "timeline"
  ]
}
)");
    // A build with the control server adds its features last.
    const std::string with = format_version_json({kControlServerFeature, kScenarioControlFeature});
    CHECK(with.find("    \"timeline\",\n    \"control-server\",\n    \"scenario-control\"\n  ]") != std::string::npos);
}

TEST_CASE(an_accepted_stop_is_asked_for_before_the_detach_and_a_refused_one_not_at_all)
{
    TimeCommands commands(false);
    Command stop;
    stop.name = "stop";
    stop.accepted = true;
    std::atomic<bool> detached{false};
    std::thread detacher;
    int asked = 0;
    // The run detaches the commands while the stop is being asked for: it waits until it is.
    const Command done = commands.apply(stop, "control_server", [&] {
        ++asked;
        detacher = std::thread([&] {
            commands.detach();
            detached = true;
        });
        std::this_thread::sleep_for(50ms);
        CHECK(!detached.load());
    });
    detacher.join();
    CHECK(done.accepted && asked == 1 && detached.load());
    // Once detached, a stop is refused and not asked for.
    const Command late = commands.apply(stop, "control_server", [&] { ++asked; });
    CHECK(!late.accepted && asked == 1);
}
