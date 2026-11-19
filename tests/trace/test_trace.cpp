// SPDX-License-Identifier: MIT
//
// vehicle_sil::trace: the rings (the newest records are kept, the rest counted as dropped), the
// file's Chrome Trace Event shape, one ring per thread, and that nothing is recorded before
// start() or after flush(). Each test case runs in a process of its own, so each starts the trace
// once. The files written at exit by the launcher and the ECUs are checked end to end by
// launch_run.trace_files.
#include "test_harness.hpp"

#include "vehicle_sil/trace/trace.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace trace = vehicle_sil::trace;

namespace {

/// A directory of its own under the system's temporary directory, removed at the end.
struct TempDir
{
    fs::path path = fs::temp_directory_path() / ("vehicle-sil-trace-" + std::to_string(std::random_device{}()));
    TempDir() = default;
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read(const fs::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

/// The lines of the file (one event per line) whose name is `name`.
std::vector<std::string> events_named(const std::string& text, const std::string& name)
{
    std::vector<std::string> out;
    std::istringstream lines(text);
    std::string line;
    const std::string key = "\"name\":\"" + name + "\"";
    while (std::getline(lines, line))
    {
        if (line.find(key) != std::string::npos)
        {
            out.push_back(line);
        }
    }
    return out;
}

std::string field(const std::string& line, const std::string& key)
{
    std::smatch m;
    if (std::regex_search(line, m, std::regex("\"" + key + "\":(-?[0-9.]+|\"[^\"]*\")")))
    {
        return m[1];
    }
    return {};
}

} // namespace

TEST_CASE(nothing_is_recorded_before_start)
{
    CHECK(!trace::enabled());
    {
        trace::Span span("before");
        trace::instant("before");
        trace::flow_out("before", 1);
        trace::set_thread_name("before");
        trace::set_process_name("before");
    }
    CHECK(!trace::flush());
}

TEST_CASE(a_ring_keeps_the_newest_records)
{
    TempDir dir;
    CHECK(trace::start(dir.path, 3)); // rounded up to 4
    CHECK(trace::enabled());
    for (std::uint64_t i = 0; i < 10; ++i)
    {
        trace::instant("ring", static_cast<std::int64_t>(i) * 1000, i);
    }
    const auto path = trace::flush();
    CHECK(path.has_value());
    const std::string text = read(*path);
    const auto kept = events_named(text, "ring");
    CHECK_EQ(kept.size(), std::size_t{4});
    for (std::size_t i = 0; i < kept.size(); ++i)
    {
        CHECK(field(kept[i], "arg") == std::to_string(6 + i));
        CHECK(field(kept[i], "vt") == std::to_string((6 + i) * 1000));
    }
    CHECK(text.find("\"events_per_thread\":4,\"dropped\":6}") != std::string::npos);
}

TEST_CASE(the_file_has_the_chrome_trace_event_shape)
{
    TempDir dir;
    CHECK(trace::start(dir.path / "made" / "by flush"));
    trace::set_process_name("ECU one");
    trace::set_process_name("not this one");
    trace::set_thread_name("main \"thread\"");
    {
        trace::Span span("work", 5'000'000, 7);
        trace::flow_out("command", trace::command_flow(3));
        span.set_arg(8);
    }
    trace::flow_in("command", trace::command_flow(3));
    trace::instant("mark", -1, 2);
    const auto path = trace::flush();
    CHECK(path.has_value());
    // The process's name, fit for a file name, and its process id.
    CHECK(std::regex_match(path->filename().string(), std::regex("trace-ECU_one-[0-9]+\\.json")));
    CHECK(path->parent_path() == dir.path / "made" / "by flush");
    const std::string text = read(*path);
    CHECK_EQ(text.rfind("{\"displayTimeUnit\":\"ns\",\"otherData\":{\"vehicle_sil_trace\":1,", 0), std::size_t{0});
    CHECK(text.size() > 4 && text.substr(text.size() - 4) == "\n]}\n");
    CHECK(text.find("\"name\":\"process_name\",\"args\":{\"name\":\"ECU_one\"}") != std::string::npos);
    CHECK(text.find("\"name\":\"thread_name\",\"args\":{\"name\":\"main \\\"thread\\\"\"}") != std::string::npos);

    const auto spans = events_named(text, "work");
    CHECK_EQ(spans.size(), std::size_t{1});
    CHECK(std::regex_match(spans[0], std::regex("\\{\"pid\":[0-9]+,\"tid\":[0-9]+,\"name\":\"work\",\"ts\":[0-9]+\\.[0-9]{3},"
                                                "\"ph\":\"X\",\"dur\":[0-9]+\\.[0-9]{3},\"args\":\\{\"vt\":5000000,\"arg\":8\\}\\},?")));
    const auto flows = events_named(text, "command");
    CHECK_EQ(flows.size(), std::size_t{2});
    CHECK(flows[0].find("\"ph\":\"s\",\"cat\":\"flow\",\"id\":6}") != std::string::npos);
    CHECK(flows[1].find("\"ph\":\"f\",\"bp\":\"e\",\"cat\":\"flow\",\"id\":6}") != std::string::npos);
    const auto marks = events_named(text, "mark");
    CHECK_EQ(marks.size(), std::size_t{1});
    CHECK(marks[0].find("\"ph\":\"i\",\"s\":\"t\",\"args\":{\"vt\":-1,\"arg\":2}}") != std::string::npos);
    // The flow starts inside the span (a flow binds to the enclosing slice), and the span's start is
    // the earliest time.
    CHECK(std::stod(field(flows[0], "ts")) >= std::stod(field(spans[0], "ts")));
    CHECK(text.find("\"dropped\":0}") != std::string::npos);
}

TEST_CASE(threads_record_into_rings_of_their_own)
{
    TempDir dir;
    CHECK(trace::start(dir.path, 1024));
    constexpr int kThreads = 4;
    constexpr int kSpans = 1000;
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([] {
            trace::set_thread_name("worker");
            for (int i = 0; i < kSpans; ++i)
            {
                const trace::Span span("spin", i);
            }
        });
    }
    for (auto& t : threads)
    {
        t.join();
    }
    const auto path = trace::flush();
    CHECK(path.has_value());
    const std::string text = read(*path);
    std::map<std::string, int> per_thread;
    for (const auto& line : events_named(text, "spin"))
    {
        ++per_thread[field(line, "tid")];
    }
    CHECK_EQ(per_thread.size(), std::size_t{kThreads});
    for (const auto& [tid, n] : per_thread)
    {
        CHECK_EQ(n, kSpans);
        CHECK(text.find("\"tid\":" + tid + ",\"name\":\"thread_name\",\"args\":{\"name\":\"worker\"}") != std::string::npos);
    }
}

TEST_CASE(nothing_is_recorded_after_flush)
{
    TempDir dir;
    CHECK(trace::start(dir.path));
    std::optional<trace::Span> open;
    open.emplace("open across the flush");
    trace::instant("before");
    const auto path = trace::flush();
    CHECK(path.has_value());
    CHECK(!trace::enabled());
    trace::instant("after");
    open.reset();
    CHECK(!trace::flush()); // once
    CHECK(!trace::start(dir.path)); // once per process
    const std::string text = read(*path);
    CHECK_EQ(events_named(text, "before").size(), std::size_t{1});
    CHECK(events_named(text, "after").empty());
    CHECK(events_named(text, "open across the flush").empty());
}

TEST_CASE(a_process_that_recorded_nothing_writes_no_file)
{
    // Such as an ECU that the launcher asks only for --describe, with VEHICLE_SIL_TRACE inherited.
    TempDir dir;
    CHECK(trace::start(dir.path));
    trace::set_process_name("idle");
    CHECK(!trace::flush());
    CHECK(!fs::exists(dir.path));
}

TEST_CASE(a_directory_that_cannot_be_written_gives_no_file)
{
    TempDir dir;
    fs::create_directories(dir.path);
    std::ofstream(dir.path / "a file") << "not a directory";
    CHECK(trace::start(dir.path / "a file" / "below"));
    trace::instant("lost");
    CHECK(!trace::flush()); // says why on stderr
}
