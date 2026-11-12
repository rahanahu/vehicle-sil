// SPDX-License-Identifier: MIT
//
// vehicle_sil::process::Process with the helper executable (process_helper.cpp): the lines of
// both streams, long lines, the working directory, the exit status, the stop request, the end of
// the processes it started (also after its own end), no inherited handle; and the same working
// directory, inheritance and time limit for run_captured().
#include "test_harness.hpp"

#include "spawn_sleeper.hpp"
#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/process.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using vehicle_sil::process::ExitStatus;
using vehicle_sil::process::Process;
using vehicle_sil::process::ProcessOptions;
using vehicle_sil::process::Stream;

namespace {

const fs::path kHelper = VEHICLE_SIL_PROCESS_HELPER;

fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_PROCESS_SCRATCH) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

/// The lines a process passed on, "out: <line>" and "err: <line>", and a wait for one of them.
class Lines
{
public:
    ProcessOptions options(std::size_t max_line = 4096)
    {
        ProcessOptions o;
        o.max_line = max_line;
        o.on_line = [this](Stream stream, std::string_view line) {
            const std::lock_guard lock(mutex_);
            lines_.push_back((stream == Stream::Output ? "out: " : "err: ") + std::string(line));
            changed_.notify_all();
        };
        return o;
    }
    std::vector<std::string> take()
    {
        const std::lock_guard lock(mutex_);
        return lines_;
    }
    /// Waits (up to 60 s, only against a hang) until `line` came.
    bool wait_for(const std::string& line)
    {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 60s, [&] { return std::find(lines_.begin(), lines_.end(), line) != lines_.end(); });
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> lines_;
};

void check_lines(const std::vector<std::string>& actual, const std::vector<std::string>& expected)
{
    if (actual != expected)
    {
        std::string text = "got:";
        for (const auto& line : actual)
        {
            text += "\n  [" + line + "]";
        }
        text += "\nexpected:";
        for (const auto& line : expected)
        {
            text += "\n  [" + line + "]";
        }
        throw vehicle_sil::unit::Failure(text);
    }
}

/// The lines of one stream, in order.
std::vector<std::string> of(const std::vector<std::string>& lines, const std::string& stream)
{
    std::vector<std::string> out;
    for (const auto& line : lines)
    {
        if (line.rfind(stream + ": ", 0) == 0)
        {
            out.push_back(line);
        }
    }
    return out;
}

/// A stop request can be sent here: always on POSIX; on Windows only with a console.
bool can_request_stop()
{
#ifdef _WIN32
    return GetConsoleWindow() != nullptr || GetConsoleCP() != 0;
#else
    return true;
#endif
}

} // namespace

TEST_CASE(process_passes_each_line_of_both_streams_without_its_line_end)
{
    Lines lines;
    Process p(kHelper, {"--out", "one\ntwo\r\nlast without end", "--err", "e1\n\ne3\n"}, lines.options());
    const auto status = p.wait_for(60s);
    CHECK(status.has_value());
    CHECK(*status == (ExitStatus{0, std::nullopt}));
    CHECK(p.finish_output(60s));
    check_lines(of(lines.take(), "out"), {"out: one", "out: two", "out: last without end"});
    check_lines(of(lines.take(), "err"), {"err: e1", "err: ", "err: e3"});
}

TEST_CASE(process_reads_the_null_device_or_what_is_written_to_its_input)
{
    Lines none;
    Process silent(kHelper, {"--read-stdin"}, none.options());
    CHECK(!silent.write_input("x")); // no input pipe
    CHECK(silent.wait_for(60s).has_value());
    CHECK(silent.finish_output(60s));
    check_lines(of(none.take(), "out"), {"out: stdin 0"});

    Lines lines;
    ProcessOptions options = lines.options();
    options.input = true;
    Process p(kHelper, {"--read-stdin"}, std::move(options));
    CHECK(p.write_input("{\"command\":\"stop\"}\n"));
    CHECK(p.write_input("x"));
    // It reads to the end: it has not ended before the input closes.
    CHECK(!p.wait_for(200ms).has_value());
    p.close_input();
    p.close_input(); // idempotent
    CHECK(!p.write_input("late"));
    const auto status = p.wait_for(60s);
    CHECK(status.has_value() && status->code == 0);
    CHECK(p.finish_output(60s));
    check_lines(of(lines.take(), "out"), {"out: stdin 20"});
}

TEST_CASE(process_passes_a_line_longer_than_the_limit_in_pieces)
{
    Lines lines;
    Process p(kHelper, {"--out", "abcdefghij\nabcd\r\nxy\n"}, lines.options(4));
    CHECK(p.wait_for(60s).has_value());
    CHECK(p.finish_output(60s));
    // "abcd\r\n" right at the limit is one line, not "abcd" and an empty one.
    check_lines(lines.take(), {"out: abcd", "out: efgh", "out: ij", "out: abcd", "out: xy"});
}

TEST_CASE(process_does_not_cut_a_utf8_character_when_it_cuts_a_long_line)
{
    Lines lines;
    // "ab" then "ü" (2 bytes) across the limit of 3, then "€" (3 bytes) whole, then a 4-byte one.
    Process p(kHelper, {"--out", "ab\xC3\xBC\xE2\x82\xAC\xF0\x9F\x98\x80\n"}, lines.options(3));
    CHECK(p.wait_for(60s).has_value());
    CHECK(p.finish_output(60s));
    // A piece of the limit's length cannot hold a 4-byte character: it is cut where it must be.
    check_lines(lines.take(), {"out: ab", "out: \xC3\xBC", "out: \xE2\x82\xAC", "out: \xF0\x9F\x98", "out: \x80"});
}

TEST_CASE(process_starts_in_its_working_directory)
{
    const fs::path dir = scratch("process working dir");
    Lines lines;
    ProcessOptions options = lines.options();
    options.working_directory = dir;
    Process p(kHelper, {"--pwd"}, std::move(options));
    CHECK(p.wait_for(60s).has_value());
    CHECK(p.finish_output(60s));
    const auto got = lines.take();
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK(fs::equivalent(fs::path(got[0].substr(5)), dir));
}

TEST_CASE(process_reports_its_exit_code_and_the_signal_that_ended_it)
{
    Lines lines;
    Process exited(kHelper, {"--exit", "3"}, lines.options());
    CHECK(exited.wait_for(60s) == (ExitStatus{3, std::nullopt}));
#ifndef _WIN32
    Process aborted(kHelper, {"--abort"}, lines.options());
    CHECK(aborted.wait_for(60s) == (ExitStatus{128 + SIGABRT, SIGABRT}));
    CHECK(vehicle_sil::process::signal_name(SIGABRT) == "SIGABRT");
#endif
}

TEST_CASE(process_stop_request_reaches_the_process)
{
    if (!can_request_stop())
    {
        throw vehicle_sil::unit::Skipped("no console: request_stop() cannot send Ctrl+Break here");
    }
    Lines lines;
    Process p(kHelper, {"--on-stop", "9", "--out", "ready\n", "--sleep-ms", "600000"}, lines.options());
    CHECK(lines.wait_for("out: ready"));
    CHECK(p.request_stop());
    CHECK(p.wait_for(60s) == (ExitStatus{9, std::nullopt}));
    CHECK(!p.request_stop()); // it has ended
}

TEST_CASE(process_kill_tree_ends_it_and_the_processes_it_started)
{
    const fs::path dir = scratch("process kill tree");
    const std::string pidFile = (dir / "grandchild.pid").string();
    Lines lines;
    Process p(kHelper, {"--spawn-sleeper", pidFile, "--ignore-stop", "--sleep-ms", "600000"}, lines.options());
    const long grandchild = vehicle_sil::test::read_pid(pidFile, 60s);
    CHECK(grandchild > 0);
    CHECK(vehicle_sil::test::process_alive(grandchild));
    CHECK(!p.others_running()); // it runs itself: nothing is left yet
    p.kill_tree();
    CHECK(p.poll().has_value());
    const bool gone = vehicle_sil::test::wait_gone(grandchild, 60s);
    if (!gone)
    {
        vehicle_sil::test::kill_one(grandchild);
    }
    CHECK(gone);
    // The grandchild held the pipes too: they close once it is gone.
    CHECK(p.finish_output(60s));
}

TEST_CASE(process_tells_that_processes_it_started_outlive_it)
{
    const fs::path dir = scratch("process others");
    const std::string pidFile = (dir / "grandchild.pid").string();
    Lines lines;
    Process p(kHelper, {"--spawn-sleeper", pidFile, "--exit", "0"}, lines.options());
    const long grandchild = vehicle_sil::test::read_pid(pidFile, 60s);
    CHECK(grandchild > 0);
    CHECK(p.wait_for(60s) == (ExitStatus{0, std::nullopt}));
    CHECK(p.others_running());
    p.kill_tree();
    const bool gone = vehicle_sil::test::wait_gone(grandchild, 60s);
    if (!gone)
    {
        vehicle_sil::test::kill_one(grandchild);
    }
    CHECK(gone);
    CHECK(!p.others_running());
}

TEST_CASE(process_ends_with_what_it_started_when_destroyed)
{
    const fs::path dir = scratch("process destroyed");
    const std::string pidFile = (dir / "child.pid").string();
    long child = 0;
    {
        Lines lines;
        Process p(kHelper, {"--pid-file", pidFile, "--ignore-stop", "--sleep-ms", "600000"}, lines.options());
        child = vehicle_sil::test::read_pid(pidFile, 60s);
        CHECK(child > 0);
    }
    const bool gone = vehicle_sil::test::wait_gone(child, 60s);
    if (!gone)
    {
        vehicle_sil::test::kill_one(child);
    }
    CHECK(gone);
}

TEST_CASE(process_inherits_no_other_handle)
{
    // A pipe whose write end the caller could hand on: once the caller closes its own, the pipe
    // ends at once, unless the running child holds the write end too.
#ifdef _WIN32
    SECURITY_ATTRIBUTES inherit{};
    inherit.nLength = sizeof(inherit);
    inherit.bInheritHandle = TRUE;
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    CHECK(CreatePipe(&read, &write, &inherit, 0) != 0);
#else
    // Without close-on-exec: a plain fork and exec would hand it on.
    int fds[2] = {-1, -1};
    CHECK(::pipe(fds) == 0);
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);
#endif
    Lines lines;
    Process p(kHelper, {"--out", "ready\n", "--sleep-ms", "600000"}, lines.options());
    CHECK(lines.wait_for("out: ready"));
#ifdef _WIN32
    CloseHandle(write);
    DWORD available = 0;
    const bool ended = PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) == 0 && GetLastError() == ERROR_BROKEN_PIPE;
    CloseHandle(read);
#else
    ::close(fds[1]);
    char byte = 0;
    const bool ended = ::read(fds[0], &byte, 1) == 0; // end of file, not EAGAIN
    ::close(fds[0]);
#endif
    p.kill_tree();
    CHECK(ended);
}

TEST_CASE(run_captured_inherits_no_other_descriptor)
{
#ifndef _WIN32
    // POSIX: the child has descriptors 0 to 2 only, so the number tells. (Windows reuses handle
    // values; run_captured() starts its child the same way as Process, which the test above checks.)
    const int fd = ::open("/dev/null", O_RDONLY);
    CHECK(fd >= 0);
    const auto captured = vehicle_sil::process::run_captured(kHelper, {"--check-handle", std::to_string(fd)});
    ::close(fd);
    CHECK(captured.standard_output == "closed\n");
#endif
}

TEST_CASE(process_cannot_start_a_missing_executable_or_in_a_missing_directory)
{
    const fs::path dir = scratch("process missing");
    CHECK(vehicle_sil::unit::throws_if<vehicle_sil::process::StartError>(
        [&] { Process p(dir / "no_such_program", {}, ProcessOptions{}); },
        [](const vehicle_sil::process::StartError& e) {
            const std::string what = e.what();
            return what.rfind("cannot start ", 0) == 0 && what.find("no_such_program") != std::string::npos &&
                   e.cause() == vehicle_sil::process::StartError::Cause::Executable;
        }));
    ProcessOptions options;
    options.working_directory = dir / "no such directory";
    CHECK(vehicle_sil::unit::throws_if<vehicle_sil::process::StartError>(
        [&] { Process p(kHelper, {}, options); },
        [](const vehicle_sil::process::StartError& e) { return std::string(e.what()).rfind("cannot start ", 0) == 0; }));
}

TEST_CASE(run_captured_starts_in_its_working_directory)
{
    const fs::path dir = scratch("captured working dir");
    vehicle_sil::process::CaptureOptions options;
    options.timeout = 60s;
    options.working_directory = dir;
    const auto captured = vehicle_sil::process::run_captured(kHelper, {"--pwd"}, options);
    CHECK(captured.exit_code == 0);
    CHECK(captured.standard_output.size() > 1);
    CHECK(fs::equivalent(fs::path(captured.standard_output.substr(0, captured.standard_output.size() - 1)), dir));
}

TEST_CASE(run_captured_time_limit_ends_what_the_process_started)
{
    const fs::path dir = scratch("captured tree");
    const std::string pidFile = (dir / "grandchild.pid").string();
    vehicle_sil::process::CaptureOptions options;
    options.timeout = 2s; // the helper sleeps for 10 minutes: only a last line against a hang
    const auto captured =
        vehicle_sil::process::run_captured(kHelper, {"--spawn-sleeper", pidFile, "--sleep-ms", "600000"}, options);
    CHECK(!captured.exit_code.has_value());
    const long grandchild = vehicle_sil::test::read_pid(pidFile, 60s);
    CHECK(grandchild > 0);
    const bool gone = vehicle_sil::test::wait_gone(grandchild, 60s);
    if (!gone)
    {
        vehicle_sil::test::kill_one(grandchild);
    }
    CHECK(gone);
}
