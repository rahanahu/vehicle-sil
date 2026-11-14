// SPDX-License-Identifier: MIT
//
// vehicle_sil::process: run_captured() with a helper executable (process_helper.cpp): the two
// streams apart, the exit code, much output on both streams at once, the time limit, the size
// limit, standard input from the null device, a path and arguments with spaces, quotes and
// non-ASCII characters, an executable that cannot be started; ChildProcess with its log file.
#include "test_harness.hpp"

#include "vehicle_sil/process/child_process.hpp"
#include "vehicle_sil/process/detail/capture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using vehicle_sil::process::CaptureOptions;
using vehicle_sil::process::Captured;
using vehicle_sil::process::run_captured;

namespace {

const fs::path kHelper = VEHICLE_SIL_PROCESS_HELPER;

/// A directory of its own for one test, empty.
fs::path scratch(const std::string& name)
{
    const fs::path dir = fs::path(VEHICLE_SIL_PROCESS_SCRATCH) / name;
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void check_text(const std::string& actual, const std::string& expected, const char* what)
{
    if (actual != expected)
    {
        throw vehicle_sil::unit::Failure(std::string(what) + ": got\n[" + actual + "]\nexpected\n[" + expected + "]");
    }
}

} // namespace

TEST_CASE(captures_stdout_and_stderr_apart_and_the_exit_code)
{
    const Captured run = run_captured(kHelper, {"--out", "to stdout\n", "--err", "to stderr\n", "--exit", "3"});
    CHECK(run.exit_code.has_value());
    CHECK_EQ(*run.exit_code, 3);
    check_text(run.standard_output, "to stdout\n", "stdout");
    check_text(run.standard_error, "to stderr\n", "stderr");
    CHECK(!run.output_truncated && !run.error_truncated);
}

TEST_CASE(reads_much_output_on_both_streams_without_blocking)
{
    // Far more than a pipe holds, on both streams: the child blocks unless both are read.
    const Captured run = run_captured(kHelper, {"--fill-err", "300000", "--fill-out", "500000"}, CaptureOptions{30s});
    CHECK(run.exit_code == 0);
    CHECK_EQ(run.standard_output.size(), std::size_t{500000});
    CHECK_EQ(run.standard_error.size(), std::size_t{300000});
    CHECK(run.standard_output.find_first_not_of('o') == std::string::npos);
}

TEST_CASE(kills_a_process_that_takes_too_long_and_keeps_what_it_wrote)
{
    // The helper must have written its lines before the time limit ends it: on a slow machine
    // its start takes a while, so the limit is scaled.
    const auto limit = scaled_timeout(300ms);
    const auto start = std::chrono::steady_clock::now();
    const Captured run =
        run_captured(kHelper, {"--out", "started\n", "--err", "waiting\n", "--sleep-ms", "60000"}, CaptureOptions{limit});
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(!run.exit_code.has_value());
    CHECK(took >= limit);
    CHECK(took < 20s);
    check_text(run.standard_output, "started\n", "stdout");
    check_text(run.standard_error, "waiting\n", "stderr");
}

TEST_CASE(keeps_at_most_max_bytes_of_each_stream)
{
    const Captured run = run_captured(kHelper, {"--fill-out", "5000", "--err", "short"}, CaptureOptions{30s, 1000});
    CHECK(run.exit_code == 0);
    CHECK_EQ(run.standard_output.size(), std::size_t{1000});
    check_text(run.standard_error, "short", "stderr");
    CHECK(run.output_truncated && !run.error_truncated);
}

TEST_CASE(keeps_the_beginning_of_stdout_and_the_end_of_stderr)
{
    const Captured run = run_captured(kHelper, {"--out", "first", "--fill-out", "5000", "--fill-err", "5000", "--err", "last"},
                                      CaptureOptions{30s, 1000});
    CHECK(run.exit_code == 0);
    CHECK(run.output_truncated && run.error_truncated);
    CHECK_EQ(run.standard_output.size(), std::size_t{1000});
    check_text(run.standard_output.substr(0, 5), "first", "the beginning of stdout");
    CHECK_EQ(run.standard_error.size(), std::size_t{1000});
    check_text(run.standard_error.substr(996), "last", "the end of stderr");
}

TEST_CASE(stops_a_process_that_writes_without_end)
{
    const auto start = std::chrono::steady_clock::now();
    const Captured run = run_captured(kHelper, {"--flood"}, CaptureOptions{300ms, 1000});
    CHECK(!run.exit_code.has_value());
    CHECK(std::chrono::steady_clock::now() - start < 20s);
    CHECK(run.output_truncated);
}

TEST_CASE(tells_a_signal_from_an_exit_code)
{
    const Captured exited = run_captured(kHelper, {"--exit", "134"}, CaptureOptions{30s});
    CHECK(exited.exit_code == 134);
    CHECK(!exited.signal.has_value());
    const Captured aborted = run_captured(kHelper, {"--abort"}, CaptureOptions{30s});
    CHECK(aborted.exit_code.has_value() && *aborted.exit_code != 0);
#ifndef _WIN32
    CHECK(aborted.signal == SIGABRT);
    CHECK(aborted.exit_code == 128 + SIGABRT);
#endif
}

TEST_CASE(gives_the_child_an_empty_standard_input)
{
#ifndef _WIN32
    // This process's own standard input is a pipe that stays open: a child that inherited it
    // would wait for its end.
    int fds[2];
    CHECK(::pipe(fds) == 0);
    CHECK(::dup2(fds[0], STDIN_FILENO) == STDIN_FILENO);
    ::close(fds[0]);
#endif
    const Captured run = run_captured(kHelper, {"--read-stdin"}, CaptureOptions{5s});
    CHECK(run.exit_code == 0);
    check_text(run.standard_output, "stdin 0\n", "stdout");
}

TEST_CASE(runs_a_path_and_arguments_with_spaces_quotes_and_non_ascii_characters)
{
    // "dir with spaces/Übung é" and "helper ü": UTF-8, made into paths the way vehicle-sil does.
    const fs::path dir = scratch("odd paths") / vehicle_sil::process::path_from_utf8("dir with spaces/\xC3\x9C" "bung \xC3\xA9");
    fs::create_directories(dir);
    fs::path copy = dir / vehicle_sil::process::path_from_utf8("helper \xC3\xBC");
    copy += kHelper.extension();
    fs::copy_file(kHelper, copy, fs::copy_options::overwrite_existing);
    const std::vector<std::string> args{"--echo-args", "two words", "a \"quoted\" word", "back\\slash\\", "a b\\",
                                        "x\\\"y", "c:\\dir with space\\",
                                        "\xC3\xA4\xC3\xB6\xC3\xBC \xE2\x82\xAC", ""};
    const Captured run = run_captured(copy, args, CaptureOptions{30s});
    CHECK(run.exit_code == 0);
    std::string expected;
    for (const auto& arg : args)
    {
        expected += arg + "\n";
    }
    check_text(run.standard_output, expected, "the arguments as the child read them");
    // utf8_from_path() gives the UTF-8 back.
    check_text(vehicle_sil::process::utf8_from_path(copy.filename()).substr(0, 9), "helper \xC3\xBC", "utf8_from_path");
}

TEST_CASE(an_executable_that_cannot_be_started_is_an_exception_naming_it)
{
    const fs::path missing = scratch("missing") / "no_such_program";
    CHECK(vehicle_sil::unit::throws_if<std::runtime_error>(
        [&] { (void)run_captured(missing, {}); },
        [&](const std::runtime_error& e) {
            const std::string what = e.what();
            return what.rfind("cannot start ", 0) == 0 && what.find("no_such_program") != std::string::npos;
        }));
}

TEST_CASE(child_process_writes_both_streams_to_its_log_file)
{
    const fs::path log = scratch("log") / "child.log";
    {
        vehicle_sil::process::ChildProcess child(kHelper, {"--out", "out\n", "--err", "err\n", "--exit", "7"}, log);
        const auto code = child.wait_for(scaled_timeout(30s));
        CHECK(code.has_value());
        CHECK_EQ(*code, 7);
    }
    std::ifstream in(log, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(text.find("out\n") != std::string::npos);
    CHECK(text.find("err\n") != std::string::npos);
}

TEST_CASE(one_read_returns_after_a_bounded_amount)
{
#ifndef _WIN32
    // A descriptor that never runs empty: one call must still return, after its reads.
    int fd = ::open("/dev/zero", O_RDONLY | O_NONBLOCK);
    CHECK(fd >= 0);
    std::string text;
    bool truncated = false;
    vehicle_sil::process::detail::Sink sink{text, std::size_t{1} << 30, truncated};
    CHECK(vehicle_sil::process::detail::read_available(fd, sink));
    CHECK_EQ(text.size(), vehicle_sil::process::detail::kReadsPerCall * 4096);
    CHECK(fd >= 0);
    ::close(fd);
#endif
    // Windows reads its pipes with PeekNamedPipe and has no descriptor that never runs empty;
    // its loop has the same bound (kReadsPerCall), not reachable from a test.
}
