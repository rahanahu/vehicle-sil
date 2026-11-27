// SPDX-License-Identifier: MIT
//
// Deterministic unit tests for `vehicle_sil_launch serve` (serve.hpp, command_input.hpp,
// events.hpp windows, checked.hpp fixed-registry refusal): no SIL Kit and no child processes.
// Uses H1 (injectable ServeRunFunction) and explicit synchronization barriers to test run
// numbering, command matching, window isolation, signal/install races, close_cut rules, and
// contamination exit deterministically.
#include "launch_test_util.hpp"

#include "vehicle_sil/launch/checked.hpp"
#include "vehicle_sil/launch/command_input.hpp"
#include "vehicle_sil/launch/events.hpp"
#include "vehicle_sil/launch/run.hpp"
#include "vehicle_sil/launch/serve.hpp"
#include "vehicle_sil/runner/control_rules.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::launch;
using namespace vehicle_sil::runner;
using namespace vehicle_sil::config;

namespace {

/// Finds a top-level (depth 1) member value in a compact JSON object.
std::optional<std::string_view> top_field(std::string_view raw, std::string_view key)
{
    int depth = 0;
    bool in_str = false;
    for (std::size_t i = 0; i < raw.size(); ++i)
    {
        const char c = raw[i];
        if (in_str)
        {
            if (c == '\\' && i + 1 < raw.size())
            {
                ++i;
            }
            else if (c == '"')
            {
                in_str = false;
            }
            continue;
        }
        if (c == '{' || c == '[')
        {
            ++depth;
            continue;
        }
        if (c == '}' || c == ']')
        {
            --depth;
            continue;
        }
        if (c == '"')
        {
            const std::size_t start = i + 1;
            ++i;
            while (i < raw.size())
            {
                if (raw[i] == '\\' && i + 1 < raw.size())
                {
                    i += 2;
                    continue;
                }
                if (raw[i] == '"')
                {
                    break;
                }
                ++i;
            }
            const std::string_view name = raw.substr(start, i - start);
            if (depth == 1 && i + 1 < raw.size() && raw[i + 1] == ':')
            {
                std::size_t vstart = i + 2;
                std::size_t vend = vstart;
                int vdepth = 0;
                bool vstr = false;
                while (vend < raw.size())
                {
                    const char vc = raw[vend];
                    if (vstr)
                    {
                        if (vc == '\\' && vend + 1 < raw.size())
                        {
                            vend += 2;
                            continue;
                        }
                        if (vc == '"')
                        {
                            vstr = false;
                        }
                    }
                    else if (vc == '"')
                    {
                        vstr = true;
                    }
                    else if (vc == '{' || vc == '[')
                    {
                        ++vdepth;
                    }
                    else if (vc == '}' || vc == ']')
                    {
                        if (vdepth == 0)
                        {
                            break;
                        }
                        --vdepth;
                    }
                    else if (vc == ',' && vdepth == 0)
                    {
                        break;
                    }
                    ++vend;
                }
                if (name == key)
                {
                    return raw.substr(vstart, vend - vstart);
                }
                i = vend - 1;
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> json_string(std::optional<std::string_view> v)
{
    if (!v || v->size() < 2 || v->front() != '"' || v->back() != '"')
    {
        return std::nullopt;
    }
    return std::string(v->substr(1, v->size() - 2));
}

std::optional<bool> json_bool(std::optional<std::string_view> v)
{
    if (!v)
    {
        return std::nullopt;
    }
    if (*v == "true")
    {
        return true;
    }
    if (*v == "false")
    {
        return false;
    }
    return std::nullopt;
}

std::optional<std::int64_t> json_int(std::optional<std::string_view> v)
{
    if (!v || v->empty() || v->front() == '"' || *v == "null" || *v == "true" || *v == "false")
    {
        return std::nullopt;
    }
    return std::stoll(std::string(*v));
}

/// Parsed JSON event line.
struct JsonLine
{
    std::string raw;
    std::string event;
    std::int64_t seq = -1;
    std::int64_t t_ms = -1;
    std::optional<std::string> run;
    std::optional<std::string> launcher;
    std::optional<std::string> command;
    std::optional<std::string> reason;
    std::optional<std::string> asked_run;
    std::optional<bool> accepted;
    std::optional<bool> launcher_ends;
    std::optional<std::int64_t> id;
    std::optional<std::string> launcher_state;
    std::optional<std::string> status_launcher_state;
    std::optional<std::string> result_status;
};

JsonLine parse_line(const std::string& raw)
{
    JsonLine out;
    out.raw = raw;
    out.event = json_string(top_field(raw, "event")).value_or("");
    out.seq = json_int(top_field(raw, "seq")).value_or(-1);
    out.t_ms = json_int(top_field(raw, "t_ms")).value_or(-1);
    out.run = json_string(top_field(raw, "run"));
    out.launcher = json_string(top_field(raw, "launcher"));
    out.command = json_string(top_field(raw, "command"));
    out.reason = json_string(top_field(raw, "reason"));
    out.asked_run = json_string(top_field(raw, "asked_run"));
    out.accepted = json_bool(top_field(raw, "accepted"));
    out.launcher_ends = json_bool(top_field(raw, "launcher_ends"));
    out.id = json_int(top_field(raw, "id"));
    out.launcher_state = json_string(top_field(raw, "launcher_state"));
    if (const auto st = top_field(raw, "status"); st && !st->empty() && st->front() == '{')
    {
        out.status_launcher_state = json_string(top_field(*st, "launcher_state"));
    }
    if (const auto res = top_field(raw, "result"); res && !res->empty() && res->front() == '{')
    {
        out.result_status = json_string(top_field(*res, "result"));
    }
    return out;
}

class CaptureStream
{
public:
    CaptureStream() : state_(std::make_shared<State>())
    {
        EventStreamOptions options;
        options.heartbeat = 0ms;
        options.now_ms = [s = state_, n = std::make_shared<std::atomic<std::int64_t>>(0)]() {
            std::function<void()> hook;
            {
                const std::lock_guard lock(s->mutex);
                hook = std::move(s->on_next_emit);
                s->on_next_emit = {};
            }
            if (hook)
            {
                hook();
            }
            return n->fetch_add(1);
        };
        stream_ = std::make_unique<EventStream>(
          [s = state_](std::string_view bytes) {
              EventWrite out;
              std::function<void()> result_hook;
              {
                  const std::lock_guard lock(s->mutex);
                  s->buffer.append(bytes);
                  std::size_t pos = 0;
                  while (true)
                  {
                      const std::size_t nl = s->buffer.find('\n', pos);
                      if (nl == std::string::npos)
                      {
                          break;
                      }
                      JsonLine parsed = parse_line(s->buffer.substr(pos, nl - pos));
                      if (parsed.event == "result" && s->on_write_result)
                      {
                          result_hook = std::move(s->on_write_result);
                          s->on_write_result = {};
                      }
                      s->lines.push_back(std::move(parsed));
                      pos = nl + 1;
                  }
                  if (pos > 0)
                  {
                      s->buffer.erase(0, pos);
                  }
                  s->changed.notify_all();
              }
              if (result_hook)
              {
                  result_hook();
              }
              out.bytes = bytes.size();
              return out;
          },
          [s = state_](const std::string& reason) {
              const std::lock_guard lock(s->mutex);
              s->closed_reason = reason;
              s->changed.notify_all();
          },
          std::move(options));
    }

    ~CaptureStream() { stream_.reset(); }

    EventStream& stream() { return *stream_; }

    std::vector<JsonLine> lines() const
    {
        const std::lock_guard lock(state_->mutex);
        return state_->lines;
    }

    JsonLine wait_for(const std::function<bool(const JsonLine&)>& pred, std::size_t from = 0, std::chrono::milliseconds limit = 5000ms)
    {
        std::unique_lock lock(state_->mutex);
        const bool ok = state_->changed.wait_for(lock, limit, [&] {
            for (std::size_t i = from; i < state_->lines.size(); ++i)
            {
                if (pred(state_->lines[i]))
                {
                    return true;
                }
            }
            return false;
        });
        CHECK(ok);
        for (std::size_t i = from; i < state_->lines.size(); ++i)
        {
            if (pred(state_->lines[i]))
            {
                return state_->lines[i];
            }
        }
        throw unit::Failure("wait_for did not find line");
    }

    std::size_t count() const
    {
        const std::lock_guard lock(state_->mutex);
        return state_->lines.size();
    }

    void set_on_next_emit(std::function<void()> fn)
    {
        const std::lock_guard lock(state_->mutex);
        state_->on_next_emit = std::move(fn);
    }

    void set_on_write_result(std::function<void()> fn)
    {
        const std::lock_guard lock(state_->mutex);
        state_->on_write_result = std::move(fn);
    }

private:
    struct State
    {
        mutable std::mutex mutex;
        std::condition_variable changed;
        std::string buffer;
        std::vector<JsonLine> lines;
        std::string closed_reason;
        std::function<void()> on_next_emit;
        std::function<void()> on_write_result;
    };
    std::shared_ptr<State> state_;
    std::unique_ptr<EventStream> stream_;
};

ServeCommand cmd(std::string_view json)
{
    auto parsed = parse_serve_command(json);
    CHECK(parsed.has_value());
    return *parsed;
}

RunSubmission submission_of(const ServeCommand& c)
{
    RunSubmission sub;
    sub.command_id = c.command.id;
    sub.request_file = c.request;
    sub.timeline = c.timeline;
    sub.intent_json = c.intent_json;
    return sub;
}

RunReport passed_report(const std::string& file = "sim.yaml")
{
    RunReport report;
    report.check.file = file;
    report.check.config.emplace();
    report.run.emplace();
    report.run->end.simulation = SimulationEnd::Ended;
    return report;
}

std::filesystem::path scratch_dir(const std::string& name)
{
    const std::filesystem::path dir = std::filesystem::path(VEHICLE_SIL_LAUNCH_SCRATCH) / "serve" / name;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir);
    return dir;
}

void write_text(const std::filesystem::path& path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary);
    CHECK(out.good());
    out << text;
}

} // namespace

TEST_CASE(serve_parse_command_reads_run_and_fields)
{
    const auto r = parse_serve_command(R"({"command":"run","id":7,"request":"bench.json","timeline":"tl.json"})");
    CHECK(r.has_value());
    CHECK(r->command.accepted);
    CHECK(r->command.name == "run");
    CHECK(r->command.id == 7);
    CHECK(r->request == "bench.json");
    CHECK(r->timeline == "tl.json");
    CHECK(!r->run.has_value());

    const auto s = parse_serve_command(R"({"command":"stop","run":"0123456789abcdef-2","id":9})");
    CHECK(s.has_value());
    CHECK(s->command.accepted);
    CHECK(s->command.is_stop());
    CHECK(s->run == "0123456789abcdef-2");
    CHECK(s->command.id == 9);

    const auto bad_run = parse_serve_command(R"({"command":"stop","run":""})");
    CHECK(bad_run.has_value());
    CHECK(!bad_run->command.accepted);
    CHECK(bad_run->command.reason == runner::reason::kInvalidArgument);

    const auto bad_req = parse_serve_command(R"({"command":"run","request":123})");
    CHECK(bad_req.has_value());
    CHECK(!bad_req->command.accepted);
    CHECK(bad_req->command.reason == runner::reason::kInvalidArgument);

    const auto unknown = parse_serve_command(R"({"command":"bogus","run":"abc-1"})");
    CHECK(unknown.has_value());
    CHECK(!unknown->command.accepted);
    CHECK(unknown->command.reason == runner::reason::kUnknownCommand);
    CHECK(unknown->run == "abc-1");

    // Non-serve parse_command still rejects "run".
    const auto cold_run = parse_command(R"({"command":"run"})");
    CHECK(cold_run.has_value());
    CHECK(!cold_run->accepted);
    CHECK(cold_run->reason == runner::reason::kUnknownCommand);
}

TEST_CASE(serve_prepare_run_applies_request_log_dir_timeline_and_control_binding)
{
    const std::filesystem::path dir = scratch_dir("prepare");
    const std::filesystem::path req_path = dir / "req.json";
    write_text(req_path, R"({"vehicle_sil_run_request":1,"start_paused":true,"speed":2})");

    RunRequest base;
    base.host.config = (dir / "sim.yaml").string();
    base.host.log_dir = dir / "logs";
    base.host.tool_directory = dir / "tools";
    base.host.stdin_commands = true;
    ControlHost control;
    control.listen = "127.0.0.1:8510";
    base.host.control = control;

    ServeCommand c = cmd(std::string(R"({"command":"run","request":")") + req_path.generic_string() + R"("})");
    const PreparedRun prepared = prepare_execution_run(base, submission_of(c), "deadbeef01234567-3");
    CHECK(prepared.usage.empty());
    CHECK(prepared.request.intent.start_paused);
    CHECK(prepared.request.intent.speed.has_value());
    CHECK(prepared.request.host.log_dir == dir / "logs" / "deadbeef01234567-3");
    CHECK(prepared.request.host.tool_directory.empty());
    CHECK(prepared.request.host.control.has_value());
    CHECK(prepared.request.host.control->run_id == "deadbeef01234567-3");
    CHECK(prepared.request.host.control->run_id_required);

    // Scenario + timeline in request/run command.
    const std::filesystem::path scen_req = dir / "scen_req.json";
    write_text(scen_req, R"({"vehicle_sil_run_request":1,"scenario":"s.yaml"})");
    ServeCommand c_tl = cmd(std::string(R"({"command":"run","request":")") + scen_req.generic_string() + R"(","timeline":"out.tl.json"})");
    const PreparedRun with_tl = prepare_execution_run(base, submission_of(c_tl), "deadbeef01234567-4");
    CHECK(with_tl.usage.empty());
    CHECK(with_tl.request.host.timeline == "out.tl.json");

    // Missing request file yields usage finding.
    ServeCommand missing = cmd(R"({"command":"run","request":"no_such_request.json"})");
    const PreparedRun bad = prepare_execution_run(base, submission_of(missing), "deadbeef01234567-5");
    CHECK(!bad.usage.empty());
    CHECK(bad.usage.front().code == "usage");
}

TEST_CASE(serve_sequential_runs_numbering_busy_and_idle_eof)
{
    CaptureStream cap;
    const std::string instance = "0123456789abcdef";
    cap.stream().hello(42, "sim.yaml", {"serve", "sim.yaml"}, instance);

    LauncherEnd end;
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    int started = 0;
    int allow_finish = 0;

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    options.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        int n = 0;
        {
            std::unique_lock lock(gate_mu);
            n = ++started;
            gate_cv.notify_all();
            gate_cv.wait(lock, [&] { return allow_finish >= n; });
        }
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });

    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
    serve.command(cmd(R"({"command":"run","id":10})"));
    const JsonLine rs1 = cap.wait_for([](const JsonLine& l) { return l.event == "run_start"; });
    CHECK(rs1.run == instance + "-1");
    CHECK(rs1.id == 10);

    // A second run while busy is refused and does not consume a sequence number.
    serve.command(cmd(R"({"command":"run","id":11})"));
    const JsonLine busy = cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 11; });
    CHECK(busy.accepted == false);
    CHECK(busy.reason == serve_reason::kBusy);
    CHECK(!busy.run.has_value());

    {
        const std::lock_guard lock(gate_mu);
        allow_finish = 1;
    }
    gate_cv.notify_all();
    const JsonLine res1 = cap.wait_for([&](const JsonLine& l) { return l.event == "result" && l.run == instance + "-1"; });
    CHECK(res1.launcher_ends == false);
    CHECK(res1.result_status == "passed");
    CHECK(res1.status_launcher_state == "unprepared");
    const std::size_t after_res1 = static_cast<std::size_t>(res1.seq) + 1;

    // Next run gets sequence 2 (not 3).
    serve.command(cmd(R"({"command":"run","id":12})"));
    const JsonLine rs2 = cap.wait_for([](const JsonLine& l) { return l.event == "run_start"; }, after_res1);
    CHECK(rs2.run == instance + "-2");
    CHECK(rs2.id == 12);
    {
        const std::lock_guard lock(gate_mu);
        allow_finish = 2;
    }
    gate_cv.notify_all();
    const JsonLine res2 = cap.wait_for([&](const JsonLine& l) { return l.event == "result" && l.run == instance + "-2"; });
    CHECK(res2.launcher_ends == false);
    CHECK(res2.status_launcher_state == "unprepared");

    serve.end_of_input();
    const ServeEnd ended = done.get();
    CHECK(ended.code == 0);
    CHECK(!ended.at_once);

    const auto all = cap.lines();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        CHECK(all[i].seq == static_cast<std::int64_t>(i));
    }
}

TEST_CASE(serve_refuses_stale_and_untargeted_stdin_commands)
{
    CaptureStream cap;
    const std::string instance = "1111222233334444";
    cap.stream().hello(42, "sim.yaml", {"serve", "sim.yaml"}, instance);

    LauncherEnd end;
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    int allow_finish = 0;
    int run2_stops = -1;

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    int run_num = 0;
    options.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        const int n = ++run_num;
        {
            std::unique_lock lock(gate_mu);
            gate_cv.wait(lock, [&] { return allow_finish >= n; });
        }
        if (n == 2)
        {
            run2_stops = lent.controls->stops().count();
        }
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });

    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    // (d) stop while idle -> run-changed
    serve.command(cmd(R"({"command":"stop","run":"1111222233334444-1","id":1})"));
    const JsonLine idle_stop = cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 1; });
    CHECK(idle_stop.accepted == false);
    CHECK(idle_stop.reason == serve_reason::kRunChanged);
    CHECK(idle_stop.asked_run == "1111222233334444-1");
    CHECK(!idle_stop.run.has_value());

    // (c) stop and pause without "run" -> run-required
    serve.command(cmd(R"({"command":"stop","id":2})"));
    const JsonLine no_run_stop = cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 2; });
    CHECK(no_run_stop.accepted == false);
    CHECK(no_run_stop.reason == serve_reason::kRunRequired);
    CHECK(!no_run_stop.asked_run.has_value());

    serve.command(cmd(R"({"command":"pause","id":3})"));
    const JsonLine no_run_pause = cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 3; });
    CHECK(no_run_pause.accepted == false);
    CHECK(no_run_pause.reason == serve_reason::kRunRequired);

    // Run 1 completes.
    serve.command(cmd(R"({"command":"run","id":10})"));
    cap.wait_for([](const JsonLine& l) { return l.event == "run_start"; });
    {
        const std::lock_guard lock(gate_mu);
        allow_finish = 1;
    }
    gate_cv.notify_all();
    const JsonLine res1 = cap.wait_for([](const JsonLine& l) { return l.event == "result"; });
    CHECK(res1.status_launcher_state == "unprepared");

    // Run 2 starts; stale stop/pause/speed for Run 1 must all be refused and never touch Run 2.
    serve.command(cmd(R"({"command":"run","id":20})"));
    const JsonLine rs2 = cap.wait_for([](const JsonLine& l) { return l.event == "run_start" && l.id == 20; });
    CHECK(rs2.run == instance + "-2");

    serve.command(cmd(R"({"command":"stop","run":"1111222233334444-1","id":21})"));
    serve.command(cmd(R"({"command":"pause","run":"1111222233334444-1","id":22})"));
    serve.command(cmd(R"({"command":"speed","value":2,"run":"1111222233334444-1","id":23})"));
    for (int cid : {21, 22, 23})
    {
        const JsonLine ref = cap.wait_for([cid](const JsonLine& l) { return l.event == "command" && l.id == cid; });
        CHECK(ref.accepted == false);
        CHECK(ref.reason == serve_reason::kRunChanged);
        CHECK(ref.asked_run == instance + "-1");
        CHECK(!ref.run.has_value());
    }

    {
        const std::lock_guard lock(gate_mu);
        allow_finish = 2;
    }
    gate_cv.notify_all();
    const JsonLine res2 = cap.wait_for([&](const JsonLine& l) { return l.event == "result" && l.run == instance + "-2"; });
    CHECK(run2_stops == 0);
    CHECK(res2.status_launcher_state == "unprepared");

    serve.end_of_input();
    CHECK(done.get().code == 0);
}

TEST_CASE(serve_window_drops_events_after_result_and_tags_state_and_log_dropped)
{
    CaptureStream cap;
    const std::string instance = "aaaa0000bbbb1111";
    cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);

    auto w1 = cap.stream().open_run(instance + "-1", 5);
    w1->phase(Phase::Running);
    w1->log("T1", "before result");
    CHECK(w1->result(format_result_event(passed_report(), false)));
    // Late events after result must be dropped.
    w1->log("T1", "late log");
    w1->phase(Phase::Ended);
    w1->registry("silkit://late");
    w1.reset();
    ExecutionStatus st;
    st.instance = instance;
    st.launcher_state = LauncherState::Unprepared;
    cap.stream().status(st);
    CHECK(cap.stream().wait_written(2000ms));

    const auto lines = cap.lines();
    bool saw_result = false;
    for (const auto& l : lines)
    {
        if (l.event == "result")
        {
            saw_result = true;
            CHECK(l.run == instance + "-1");
            continue;
        }
        if (saw_result)
        {
            CHECK(!l.run.has_value());
        }
    }
    CHECK(saw_result);
}

TEST_CASE(serve_signal_during_run_install_never_starts_unstopped_run)
{
    // A-M1: simulate a signal arriving while the run is being installed (both before publish()
    // and immediately after): Serve::run() re-reads end_.requested() (seq_cst) before calling
    // options_.run(), so the run always sees at least one stop request.
    CaptureStream cap;
    const std::string instance = "5555666677778888";
    cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);

    LauncherEnd end;
    int seen_stops = -1;
    bool close_cut_val = false;
    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    options.run = [&](const RunRequest&, const RunObservers& obs, const RunLent& lent) {
        seen_stops = lent.controls->stops().count();
        close_cut_val = obs.close_cut(0);
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });

    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
    // Inject a signal when open_run() emits run_start inside ExecutionRuntime::start_run() right
    // before end_.publish(&run->stops): LauncherEnd::request() sees run_ == nullptr, marks end_,
    // then start_run() publishes and hands the run to run_loop(), which re-checks end_.requested()
    // and stops the run before calling options_.run().
    cap.set_on_next_emit([&] { end.request(2); });
    serve.command(cmd(R"({"command":"run","id":1})"));

    const ServeEnd ended = done.get();
    CHECK(ended.code == kExitInterrupted);
    CHECK(!ended.at_once);
    CHECK(seen_stops >= 1);
    CHECK(close_cut_val);

    // Result must have launcher_ends: true and nothing after it.
    const auto lines = cap.lines();
    bool saw_res = false;
    for (const auto& l : lines)
    {
        if (l.event == "result")
        {
            CHECK(l.launcher_ends == true);
            saw_res = true;
        }
        else if (saw_res)
        {
            CHECK(false);
        }
    }
    CHECK(saw_res);
}

TEST_CASE(serve_close_cut_ignores_first_run_stop_and_cuts_on_second_or_launcher_end)
{
    CaptureStream cap;
    const std::string instance = "9999aaaabbbbcccc";
    cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);

    LauncherEnd end;
    bool cut_0 = true;
    bool cut_1 = true;
    bool cut_2_during_close = false;
    bool cut_2_before_close = true;

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    options.run = [&](const RunRequest&, const RunObservers& obs, const RunLent& lent) {
        cut_0 = obs.close_cut(0);
        lent.controls->stops().request(stop_source::kStopCommand);
        cut_1 = obs.close_cut(0);
        lent.controls->stops().request(stop_source::kStopCommand);
        cut_2_during_close = obs.close_cut(0);
        cut_2_before_close = obs.close_cut(2);
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
    serve.command(cmd(R"({"command":"run","id":1})"));
    const JsonLine res = cap.wait_for([](const JsonLine& l) { return l.event == "result"; });
    CHECK(res.launcher_ends == false);
    CHECK(res.status_launcher_state == "unprepared");
    serve.end_of_input();
    CHECK(done.get().code == 0);

    CHECK(!cut_0);
    CHECK(!cut_1);
    CHECK(cut_2_during_close);
    CHECK(!cut_2_before_close);
}

TEST_CASE(serve_contamination_not_closed_ms_and_exception_end_launcher_at_once)
{
    // Case 1 (H1): RunOutcome::close.not_closed_ms set -> kExitContaminated, at_once=true, no idle.
    {
        CaptureStream cap;
        const std::string instance = "ccccddddeeeeffff";
        cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);
        LauncherEnd end;
        ServeOptions options;
        options.instance = instance;
        options.base.host.config = "sim.yaml";
        options.base.host.stdin_commands = true;
        options.run = [](const RunRequest&, const RunObservers&, const RunLent& lent) {
            RunReport r = passed_report();
            r.run->close.not_closed_ms = 5000;
            r.findings.push_back(Finding{Severity::Error, "connection-not-closed", "connection did not close", 0, "", ""});
            lent.controls->set_phase(Phase::Ended);
            return r;
        };
        Serve serve(cap.stream(), end, std::move(options));
        auto done = std::async(std::launch::async, [&] { return serve.run(); });
        cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
        serve.command(cmd(R"({"command":"run","id":1})"));
        const ServeEnd ended = done.get();
        CHECK(ended.code == kExitContaminated);
        CHECK(ended.at_once);
        const auto lines_before = cap.lines();
        CHECK(lines_before.back().event == "result");
        CHECK(lines_before.back().launcher_ends == true);
        // M1: stream is sealed after launcher_ends: true; late commands emit nothing after result.
        serve.command(cmd(R"({"command":"run","id":2})"));
        std::this_thread::sleep_for(30ms);
        CHECK(cap.lines().size() == lines_before.size());
    }

    // Case 2 (H1): run function throws -> kExitContaminated, at_once=true, no idle.
    {
        CaptureStream cap;
        const std::string instance = "fedcba9876543210";
        cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);
        LauncherEnd end;
        ServeOptions options;
        options.instance = instance;
        options.base.host.config = "sim.yaml";
        options.base.host.stdin_commands = true;
        options.run = [](const RunRequest&, const RunObservers&, const RunLent&) -> RunReport {
            throw std::runtime_error("injected internal error");
        };
        Serve serve(cap.stream(), end, std::move(options));
        auto done = std::async(std::launch::async, [&] { return serve.run(); });
        cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
        serve.command(cmd(R"({"command":"run","id":1})"));
        const ServeEnd ended = done.get();
        CHECK(ended.code == kExitContaminated);
        CHECK(ended.at_once);
        const auto lines = cap.lines();
        CHECK(lines.back().event == "result");
        CHECK(lines.back().launcher_ends == true);
    }

    // Case 3 (H1/H2/M1): run sent BEFORE Serve::run() still orders initial status before run_start,
    // and EOF right after contaminated result does not overwrite kExitContaminated with 0.
    {
        CaptureStream cap;
        const std::string instance = "1234567890abcdef";
        cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);
        LauncherEnd end;
        ServeOptions options;
        options.instance = instance;
        options.base.host.config = "sim.yaml";
        options.base.host.stdin_commands = true;
        options.run = [](const RunRequest&, const RunObservers&, const RunLent& lent) {
            RunReport r = passed_report();
            r.run->close.not_closed_ms = 5000;
            lent.controls->set_phase(Phase::Ended);
            return r;
        };
        Serve serve(cap.stream(), end, std::move(options));
        // Command arrives BEFORE serve.run() is entered!
        serve.command(cmd(R"({"command":"run","id":99})"));
        // EOF also arrives after contamination (before or during result write): must still exit 6.
        const ServeEnd ended = serve.run();
        serve.end_of_input();
        CHECK(ended.code == kExitContaminated);
        CHECK(ended.at_once);
        const auto lines = cap.lines();
        CHECK(lines.size() >= 4);
        CHECK(lines[0].event == "hello");
        CHECK(lines[1].event == "status");
        CHECK(lines[1].launcher_state == "unprepared");
        CHECK(lines[2].event == "run_start");
        CHECK(lines.back().event == "result");
        CHECK(lines.back().launcher_ends == true);
    }

    // Case 4 (M1): run command arriving after end_ is requested (while Run 1 is still active) is
    // refused with launcher-ending before the final result seals the stream.
    {
        CaptureStream cap;
        const std::string instance = "a1b2c3d4e5f60718";
        cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);
        LauncherEnd end;
        std::mutex mu;
        std::condition_variable cv;
        bool in_run = false;
        bool release = false;
        ServeOptions options;
        options.instance = instance;
        options.base.host.config = "sim.yaml";
        options.base.host.stdin_commands = true;
        options.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
            {
                const std::lock_guard lock(mu);
                in_run = true;
            }
            cv.notify_all();
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return release; });
            lent.controls->set_phase(Phase::Ended);
            return passed_report();
        };
        Serve serve(cap.stream(), end, std::move(options));
        auto done = std::async(std::launch::async, [&] { return serve.run(); });
        cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
        serve.command(cmd(R"({"command":"run","id":1})"));
        {
            std::unique_lock lock(mu);
            cv.wait(lock, [&] { return in_run; });
        }
        end.request(2); // signal marks launcher ending before Run 1 finishes
        serve.command(cmd(R"({"command":"run","id":2})"));
        const JsonLine ref = cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 2; });
        CHECK(ref.accepted == false);
        CHECK(ref.reason == serve_reason::kLauncherEnding);
        {
            const std::lock_guard lock(mu);
            release = true;
        }
        cv.notify_all();
        const ServeEnd ended = done.get();
        CHECK(ended.code == kExitInterrupted);
        CHECK(cap.lines().back().event == "result");
        CHECK(cap.lines().back().launcher_ends == true);
    }
}

TEST_CASE(serve_eof_during_run_and_output_closed_return_interrupted)
{
    CaptureStream cap;
    const std::string instance = "0011223344556677";
    cap.stream().hello(1, "sim.yaml", {"serve", "sim.yaml"}, instance);

    LauncherEnd end;
    std::mutex mu;
    std::condition_variable cv;
    bool in_run = false;
    bool release = false;

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    options.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        {
            const std::lock_guard lock(mu);
            in_run = true;
        }
        cv.notify_all();
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return release; });
        CHECK(lent.controls->stops().count() == 1);
        CHECK(lent.controls->stops().first() == stop_source::kEndOfInput);
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });
    serve.command(cmd(R"({"command":"run","id":1})"));
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run; });
    }
    serve.end_of_input();
    {
        const std::lock_guard lock(mu);
        release = true;
    }
    cv.notify_all();
    const ServeEnd ended = done.get();
    CHECK(ended.code == kExitInterrupted);
    CHECK(!ended.at_once);
    const auto lines = cap.lines();
    CHECK(lines.back().event == "result");
    CHECK(lines.back().launcher_ends == true);
}

TEST_CASE(serve_refuses_fixed_registry_uri_and_manual_before_describe)
{
    const std::filesystem::path dir = scratch_dir("fixed");
    const std::filesystem::path fixed_uri = dir / "fixed_uri.yaml";
    // Executable does not exist: if --describe ran, it would report an executable error instead of usage.
    write_text(fixed_uri,
               "vehicle_sil_simulation: 2\n"
               "registry_uri: silkit://127.0.0.1:8500\n"
               "networks:\n  - name: BUS\n    kind: can\n"
               "participants:\n  - name: A\n    command: [\"./does_not_exist_ecu\"]\n");
    RunRequest req1;
    req1.host.config = fixed_uri.string();
    CheckOptions opts;
    opts.refuse_fixed_registry = true;
    const Checked c1 = check_file(req1, opts);
    CHECK(c1.usage);
    CHECK(!c1.findings.empty());
    CHECK(c1.findings.front().code == "usage");
    CHECK(c1.findings.front().message.find("registry_uri") != std::string::npos);

    const std::filesystem::path manual_cfg = dir / "manual.yaml";
    write_text(manual_cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: BUS\n    kind: can\n"
               "participants:\n  - name: A\n    command: [\"./does_not_exist_ecu\"]\n    manual: true\n");
    RunRequest req2;
    req2.host.config = manual_cfg.string();
    const Checked c2 = check_file(req2, opts);
    CHECK(c2.usage);
    CHECK(!c2.findings.empty());
    CHECK(c2.findings.front().code == "usage");
    CHECK(c2.findings.front().message.find("manual") != std::string::npos);
}

TEST_CASE(serve_parse_command_reads_prepare_status_force_and_inline_intent)
{
    // prepare: valid with variant and disable/enable/manual (including empty arrays).
    const auto prep = parse_serve_command(
        R"({"command":"prepare","id":11,"variant":"fast","disable":["Logger"],"enable":[],"manual":["Dash"]})");
    CHECK(prep.has_value());
    CHECK(prep->command.accepted);
    CHECK(prep->command.name == "prepare");
    CHECK(prep->command.id == 11);
    CHECK(prep->variant == "fast");
    CHECK((prep->disable == std::vector<std::string>{"Logger"}));
    CHECK(prep->enable.empty());
    CHECK((prep->manual == std::vector<std::string>{"Dash"}));

    // prepare rejects explicit "config" key and invalid types.
    const auto prep_cfg = parse_serve_command(R"({"command":"prepare","config":"other.yaml"})");
    CHECK(prep_cfg.has_value());
    CHECK(!prep_cfg->command.accepted);
    CHECK(prep_cfg->command.reason == runner::reason::kInvalidArgument);

    const auto prep_bad_var = parse_serve_command(R"({"command":"prepare","variant":123})");
    CHECK(prep_bad_var.has_value());
    CHECK(!prep_bad_var->command.accepted);
    CHECK(prep_bad_var->command.reason == runner::reason::kInvalidArgument);

    const auto prep_bad_dis = parse_serve_command(R"({"command":"prepare","disable":["", "A"]})");
    CHECK(prep_bad_dis.has_value());
    CHECK(!prep_bad_dis->command.accepted);
    CHECK(prep_bad_dis->command.reason == runner::reason::kInvalidArgument);

    // status: valid and invalid id.
    const auto st = parse_serve_command(R"({"command":"status","id":12})");
    CHECK(st.has_value());
    CHECK(st->command.accepted);
    CHECK(st->command.name == "status");
    CHECK(st->command.id == 12);

    const auto st_bad = parse_serve_command(R"({"command":"status","id":"abc"})");
    CHECK(st_bad.has_value());
    CHECK(!st_bad->command.accepted);
    CHECK(st_bad->command.reason == runner::reason::kInvalidArgument);

    // stop: force true, false, omitted, invalid.
    const auto stop_force = parse_serve_command(R"({"command":"stop","run":"r-1","force":true,"id":3})");
    CHECK(stop_force.has_value() && stop_force->command.accepted && stop_force->force == std::optional<bool>(true));
    const auto stop_grace = parse_serve_command(R"({"command":"stop","run":"r-1","force":false})");
    CHECK(stop_grace.has_value() && stop_grace->command.accepted && stop_grace->force == std::optional<bool>(false));
    const auto stop_esc = parse_serve_command(R"({"command":"stop","run":"r-1"})");
    CHECK(stop_esc.has_value() && stop_esc->command.accepted && !stop_esc->force.has_value());
    const auto stop_bad = parse_serve_command(R"({"command":"stop","run":"r-1","force":"yes"})");
    CHECK(stop_bad.has_value() && !stop_bad->command.accepted &&
          stop_bad->command.reason == runner::reason::kInvalidArgument);

    // run: inline intent object, mutual exclusion with request, type rejection.
    const auto run_intent = parse_serve_command(
        R"({"command":"run","id":4,"timeline":"tl.json","intent":{"speed":2,"start_paused":true}})");
    CHECK(run_intent.has_value() && run_intent->command.accepted);
    CHECK(run_intent->timeline == "tl.json");
    CHECK(run_intent->intent_json.has_value());

    const auto run_both =
        parse_serve_command(R"({"command":"run","request":"a.json","intent":{"speed":2}})");
    CHECK(run_both.has_value() && !run_both->command.accepted &&
          run_both->command.reason == runner::reason::kInvalidArgument);

    const auto run_bad_intent = parse_serve_command(R"({"command":"run","intent":["bad"]})");
    CHECK(run_bad_intent.has_value() && !run_bad_intent->command.accepted &&
          run_bad_intent->command.reason == runner::reason::kInvalidArgument);

    // parse_command (non-serve) continues rejecting prepare and status.
    CHECK(!parse_command(R"({"command":"prepare"})")->accepted);
    CHECK(parse_command(R"({"command":"prepare"})")->reason == runner::reason::kUnknownCommand);
    CHECK(!parse_command(R"({"command":"status"})")->accepted);
    CHECK(parse_command(R"({"command":"status"})")->reason == runner::reason::kUnknownCommand);

    // 64 KiB boundary in CommandInput(Serve{}, ...).
    std::vector<ServeCommand> got;
    CommandInput input(CommandInput::Serve{}, [&](const ServeCommand& c) { got.push_back(c); });
    const std::string prefix = R"({"command":"run","intent":{"scenario":")";
    const std::string suffix = R"("}})";
    const std::string exact_ok = prefix + std::string(kMaxCommandLine - prefix.size() - suffix.size(), 'x') + suffix;
    CHECK_EQ(exact_ok.size(), kMaxCommandLine);
    const std::string too_long = exact_ok + "x";
    input.append(exact_ok + "\n" + too_long + "\n" + R"({"command":"status","id":99})" + "\n");
    input.finish();
    CHECK_EQ(got.size(), std::size_t{3});
    CHECK(got[0].command.accepted && got[0].intent_json.has_value());
    CHECK(!got[1].command.accepted && got[1].command.reason == runner::reason::kInvalidCommand);
    CHECK(got[2].command.accepted && got[2].command.name == "status" && got[2].command.id == 99);
}

TEST_CASE(serve_prepare_run_handles_inline_intent_with_and_without_format_version)
{
    const std::filesystem::path dir = scratch_dir("prepare_intent");
    RunRequest base;
    base.host.config = (dir / "sim.yaml").string();
    base.host.log_dir = dir / "logs";
    base.host.stdin_commands = true;

    // Without vehicle_sil_run_request (optional for inline intent).
    ServeCommand c1 = cmd(R"({"command":"run","intent":{"speed":2,"scenario":"drive.yaml"}})");
    const PreparedRun p1 = prepare_execution_run(base, submission_of(c1), "0011223344556677-1");
    CHECK(p1.usage.empty());
    CHECK(!p1.request.intent.start_paused);
    CHECK(p1.request.intent.speed == std::optional<std::optional<double>>(std::optional<double>(2.0)));
    CHECK(p1.request.intent.scenario == std::optional<std::string>("drive.yaml"));
    CHECK(p1.request.origin.has("scenario"));
    CHECK(p1.request.origin.file == "intent");

    // With explicit vehicle_sil_run_request: 1.
    ServeCommand c2 = cmd(R"({"command":"run","intent":{"vehicle_sil_run_request":1,"start_paused":true,"strict":true}})");
    const PreparedRun p2 = prepare_execution_run(base, submission_of(c2), "0011223344556677-2");
    CHECK(p2.usage.empty());
    CHECK(p2.request.intent.start_paused);
    CHECK(p2.request.intent.strict);

    // Wrong vehicle_sil_run_request version or unknown key yields usage finding.
    ServeCommand c_bad_ver = cmd(R"({"command":"run","intent":{"vehicle_sil_run_request":2}})");
    const PreparedRun p_bad_ver = prepare_execution_run(base, submission_of(c_bad_ver), "0011223344556677-3");
    CHECK(!p_bad_ver.usage.empty());
    CHECK(p_bad_ver.usage.front().code == "usage");

    ServeCommand c_bad_key = cmd(R"({"command":"run","intent":{"bogus":1}})");
    const PreparedRun p_bad_key = prepare_execution_run(base, submission_of(c_bad_key), "0011223344556677-4");
    CHECK(!p_bad_key.usage.empty());
    CHECK(p_bad_key.usage.front().code == "usage");
}

TEST_CASE(serve_executes_prepare_status_stop_force_and_inline_intent)
{
    const std::filesystem::path dir = scratch_dir("serve_ext");
    const std::filesystem::path cfg = dir / "sim.yaml";
    write_text(cfg,
               "vehicle_sil_simulation: 2\n"
               "networks:\n  - name: CAN1\n    kind: can\n"
               "participants:\n  - name: Engine\n    command: [\"./engine\"]\n");

    CaptureStream cap;
    LauncherEnd end;
    const std::string instance = "fedcba9876543210";
    cap.stream().hello(100, cfg.string(), {"serve", cfg.string()}, instance);

    std::mutex mu;
    std::condition_variable cv;
    bool in_run = false;
    bool release_run = false;
    int seen_stop_count = 0;
    bool seen_start_paused = false;

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = cfg.string();
    options.base.host.stdin_commands = true;
    options.run = [&](const RunRequest& req, const RunObservers&, const RunLent& lent) {
        {
            const std::lock_guard lock(mu);
            seen_start_paused = req.intent.start_paused;
            in_run = true;
        }
        cv.notify_all();
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return release_run; });
        seen_stop_count = lent.controls->stops().count();
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    // 1. status while unprepared.
    serve.command(cmd(R"({"command":"status","id":1})"));
    const JsonLine st1 = cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.id == 1; });
    CHECK(st1.id == 1);
    CHECK(!st1.run.has_value());
    CHECK(json_string(top_field(st1.raw, "launcher_state")) == "unprepared");

    // 2. prepare while unprepared -> ready, eligible: true, file_sha256 populated.
    serve.command(cmd(R"({"command":"prepare","id":2})"));
    const JsonLine prep1 = cap.wait_for([](const JsonLine& l) { return l.event == "prepare"; });
    CHECK(prep1.id == 2);
    CHECK(prep1.accepted == true);
    CHECK(!prep1.run.has_value());
    CHECK(prep1.status_launcher_state == "ready");

    // 3. run with inline intent; verify status during run and stop with force=false twice (stays 1).
    serve.command(cmd(R"({"command":"run","id":3,"intent":{"start_paused":true}})"));
    {
        std::unique_lock lock(mu);
        cv.wait(lock, [&] { return in_run; });
    }
    serve.command(cmd(R"({"command":"status","id":4})"));
    const JsonLine st2 = cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.id == 4; });
    CHECK(json_string(top_field(st2.raw, "launcher_state")) == "running");
    CHECK(json_string(top_field(st2.raw, "active_run_id")) == instance + "-1");

    // prepare while running is refused with busy.
    serve.command(cmd(R"({"command":"prepare","id":5})"));
    const JsonLine prep_busy = cap.wait_for([](const JsonLine& l) { return l.event == "prepare" && l.id == 5; });
    CHECK(prep_busy.accepted == false);
    CHECK(prep_busy.reason == serve_reason::kBusy);

    // Two stop commands with force: false (or omitted) MUST stay at count == 1 (idempotent Graceful).
    const std::string run_id = instance + "-1";
    serve.command(cmd(std::string(R"({"command":"stop","run":")") + run_id + R"(","force":false,"id":6})"));
    cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 6; });
    serve.command(cmd(std::string(R"({"command":"stop","run":")") + run_id + R"(","id":7})"));
    cap.wait_for([](const JsonLine& l) { return l.event == "command" && l.id == 7; });
    {
        const std::lock_guard lock(mu);
        release_run = true;
    }
    cv.notify_all();

    const JsonLine run1_res = cap.wait_for([&](const JsonLine& l) { return l.event == "result" && l.run == run_id; });
    CHECK(run1_res.status_launcher_state == "ready");
    CHECK(seen_start_paused);
    CHECK_EQ(seen_stop_count, 1);

    // Invalid inline intent emits usage result and returns to ready.
    serve.command(cmd(R"({"command":"run","id":8,"intent":{"unknown_key":1}})"));
    const JsonLine bad_res = cap.wait_for([&](const JsonLine& l) { return l.event == "result" && l.run == instance + "-2"; });
    CHECK(bad_res.result_status == "usage");
    CHECK(bad_res.launcher_ends == false);
    CHECK(bad_res.status_launcher_state == "ready");

    serve.end_of_input();
    const ServeEnd ended = done.get();
    CHECK_EQ(ended.code, 0);
}

TEST_CASE(serve_single_run_exits_zero_after_one_run_with_launcher_ends_true)
{
    CaptureStream cap;
    LauncherEnd end;
    const std::string instance = "1122334455667788";
    cap.stream().hello(100, "sim.yaml", {"serve", "--once", "sim.yaml"}, instance);

    ServeOptions options;
    options.instance = instance;
    options.base.host.config = "sim.yaml";
    options.base.host.stdin_commands = true;
    options.single_run = true;
    options.run = [](const RunRequest&, const RunObservers&, const RunLent& lent) {
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    serve.command(cmd(R"({"command":"run","id":1})"));
    const ServeEnd ended = done.get();
    CHECK_EQ(ended.code, 0);
    CHECK(!ended.at_once);

    const auto lines = cap.lines();
    CHECK(lines.back().event == "result");
    CHECK(lines.back().launcher_ends == true);
    CHECK(lines.back().status_launcher_state == "ending");
}

TEST_CASE(serve_concurrent_next_run_and_eof_during_result_flush_preserve_strict_order)
{
    CaptureStream cap;
    LauncherEnd end;
    ServeOptions options;
    options.instance = "ccddeeff00112233";
    options.base.host.config = "sim.yaml";
    std::atomic<int> runs{0};
    options.run = [&](const RunRequest&, const RunObservers&, const RunLent& lent) {
        ++runs;
        lent.controls->set_phase(Phase::Running);
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    // When Run 1's result is written to the output callback (while run_loop() is still blocked in
    // finish_run()'s wait_result()), immediately submit Run 2; when Run 2's result is written,
    // immediately signal end_of_input().
    cap.set_on_write_result([&] {
        cap.set_on_write_result([&] { serve.end_of_input(); });
        serve.command(cmd(R"({"command":"run","id":2})"));
    });
    serve.command(cmd(R"({"command":"run","id":1})"));

    const ServeEnd ended = done.get();
    CHECK_EQ(ended.code, 0);
    CHECK_EQ(runs.load(), 2);

    const auto lines = cap.lines();
    std::size_t r1_result = lines.size();
    std::size_t r2_start = lines.size();
    std::size_t r2_result = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i)
    {
        if (lines[i].event == "result" && lines[i].run == "ccddeeff00112233-1")
        {
            r1_result = i;
        }
        if (lines[i].event == "run_start" && lines[i].run == "ccddeeff00112233-2")
        {
            r2_start = i;
        }
        if (lines[i].event == "result" && lines[i].run == "ccddeeff00112233-2")
        {
            r2_result = i;
        }
    }
    CHECK(r1_result < r2_start);
    CHECK(r2_start < r2_result);
    CHECK(lines.back().event == "status");
    CHECK(lines.back().launcher_state == "ending");
}

TEST_CASE(serve_rejects_multi_case_scenario_intent_without_case)
{
    const std::filesystem::path dir = scratch_dir("serve_multicase_no_case");
    {
        std::ofstream out(dir / "sim.yaml", std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "networks: []\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [missing/engine]\n"
               "    describe: false\n";
    }
    {
        std::ofstream out(dir / "cases.yaml", std::ios::binary);
        out << "vehicle_sil_scenario: 1\n"
               "cases:\n"
               "  low: { params: { lvl: 10 } }\n"
               "  high: { params: { lvl: 90 } }\n"
               "steps:\n"
               "  - set: { Engine.level: \"${lvl}\" }\n";
    }

    CaptureStream cap;
    LauncherEnd end;
    ServeOptions options;
    options.instance = "11223344aabbccdd";
    options.base.host.config = (dir / "sim.yaml").string();
    options.base.host.log_dir = dir / "logs";
    options.base.host.stdin_commands = true;
    options.single_run = true;

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    serve.command(cmd(R"({"command":"run","id":1,"intent":{"scenario":"cases.yaml"}})"));
    const ServeEnd ended = done.get();
    CHECK_EQ(ended.code, 4);

    const auto lines = cap.lines();
    const auto res = std::find_if(lines.begin(), lines.end(), [](const JsonLine& l) { return l.event == "result"; });
    CHECK(res != lines.end());
    CHECK(res->raw.find(R"("ok":false)") != std::string::npos);
    CHECK(res->raw.find("--case") != std::string::npos);
    CHECK(res->raw.find("low, high") != std::string::npos);
    // Preserved check metadata on multi-case usage: case is null, cases and participants are kept.
    CHECK(res->raw.find(R"("case":null)") != std::string::npos);
    CHECK(res->raw.find(R"("cases":["low","high"])") != std::string::npos);
    CHECK(res->raw.find(R"("eligible":true)") != std::string::npos);
}

TEST_CASE(serve_intent_supports_case_cli_params_and_keeps_ready_across_case_switches)
{
    const std::filesystem::path dir = scratch_dir("serve_case_cli_params");
    std::filesystem::create_directories(dir / "templates");
    {
        std::ofstream out(dir / "sim.yaml", std::ios::binary);
        out << "vehicle_sil_simulation: 2\n"
               "networks: []\n"
               "participants:\n"
               "  - name: Engine\n"
               "    command: [missing/engine]\n"
               "    describe: false\n";
    }
    {
        std::ofstream out(dir / "templates" / "typed.yaml", std::ios::binary);
        out << "vehicle_sil_scenario_template: 1\n"
               "parameters:\n"
               "  lvl: { type: integer }\n"
               "  code: { type: string }\n"
               "steps:\n"
               "  - name: \"Code ${code}\"\n"
               "    set: { Engine.level: \"${lvl}\" }\n";
    }
    {
        std::ofstream out(dir / "cases.yaml", std::ios::binary);
        out << "vehicle_sil_scenario: 1\n"
               "cases:\n"
               "  low: { params: { lvl: 10, code: \"0001\" } }\n"
               "  high: { params: { lvl: 90, code: \"0002\" } }\n"
               "steps:\n"
               "  - template: templates/typed.yaml\n"
               "    params: { lvl: \"${lvl}\", code: \"${code}\" }\n";
    }

    // 1. Parse serve NDJSON command -> prepare_execution_run -> check_file preserves case & cli_params verbatim.
    const ServeCommand parsed = cmd(
        R"({"command":"run","id":1,"intent":{"scenario":"cases.yaml","case":"low","params":{"lvl":20},"cli_params":{"lvl":"0x10","code":"0123"}}})");
    CHECK(parsed.intent_json.has_value());
    RunSubmission sub;
    sub.intent_json = parsed.intent_json;
    RunRequest base;
    base.host.config = (dir / "sim.yaml").string();
    const PreparedRun prepared = prepare_execution_run(base, sub, "inst-1", false);
    CHECK(prepared.usage.empty());
    CHECK(prepared.request.intent.scenario_case == std::optional<std::string>("low"));
    const Checked checked = check_file(prepared.request);
    CHECK(!checked.usage && checked.scenario.has_value());
    CHECK(checked.scenario->case_name == "low");
    const std::string checkedJson = format_json(checked);
    CHECK(checkedJson.find("\"value\": 16") != std::string::npos);
    CHECK(checkedJson.find("\"value\": \"0123\"") != std::string::npos);

    // 2. Switching scenario/case/params across sequential serve runs preserves hot-standby Ready state.
    CaptureStream cap;
    LauncherEnd end;
    ServeOptions options;
    options.instance = "abcdef0123456789";
    options.base.host.config = (dir / "sim.yaml").string();
    std::vector<std::optional<std::string>> seen_cases;
    options.run = [&](const RunRequest& req, const RunObservers&, const RunLent& lent) {
        seen_cases.push_back(req.intent.scenario_case);
        lent.controls->set_phase(Phase::Running);
        lent.controls->set_phase(Phase::Ended);
        return passed_report();
    };

    Serve serve(cap.stream(), end, std::move(options));
    auto done = std::async(std::launch::async, [&] { return serve.run(); });
    cap.wait_for([](const JsonLine& l) { return l.event == "status" && l.launcher_state == "unprepared"; });

    serve.command(cmd(R"({"command":"prepare","id":1})"));
    cap.wait_for([](const JsonLine& l) { return l.event == "prepare" && l.id == 1; });

    serve.command(cmd(R"({"command":"run","id":2,"intent":{"scenario":"cases.yaml","case":"low","params":{"lvl":11}}})"));
    const JsonLine r1 = cap.wait_for([](const JsonLine& l) { return l.event == "result" && l.run == "abcdef0123456789-1"; });
    CHECK(r1.status_launcher_state == "ready");

    serve.command(cmd(R"({"command":"run","id":3,"intent":{"scenario":"cases.yaml","case":"high","cli_params":{"code":"50"}}})"));
    const JsonLine r2 = cap.wait_for([](const JsonLine& l) { return l.event == "result" && l.run == "abcdef0123456789-2"; });
    CHECK(r2.status_launcher_state == "ready");

    serve.end_of_input();
    CHECK_EQ(done.get().code, 0);
    CHECK_EQ(seen_cases.size(), std::size_t{2});
    CHECK(seen_cases[0] == std::optional<std::string>("low"));
    CHECK(seen_cases[1] == std::optional<std::string>("high"));
}
