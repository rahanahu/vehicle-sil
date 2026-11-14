// SPDX-License-Identifier: MIT
//
// ecu_main() called as a function: the usage (no arguments, --help), every bad command line with
// its exit code and message, --describe (the JSON of the ECU, --name, a set-up that writes to
// stdout, a closed stdout), --local (runs the ECU, a failed step), a failed set-up (also with an
// exception that is not a std::exception, a reserved variable name, a name that is not UTF-8), a
// registry that cannot be reached, a start that times out (--start-timeout-ms) or fails, the
// ECU's variables offered to a hub over SIL Kit (and --no-control), and the template form passing
// its arguments to the application.
#include "silkit_test_util.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/control/hub.hpp"
#include "vehicle_sil/control_silkit/hub_transport.hpp"
#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/core/simulation_controller.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <random>
#include <system_error>
#include <memory>
#include <thread>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace std::chrono_literals;
using vehicle_sil::test::scaled_timeout;
using vehicle_sil::Ecu;
using vehicle_sil::EcuMainOptions;
using vehicle_sil::SimTime;
using vehicle_sil::to_json;

namespace {

#ifdef _WIN32
int dup_fd(int fd) { return _dup(fd); }
int dup2_fd(int from, int to) { return _dup2(from, to); }
void close_fd(int fd) { static_cast<void>(_close(fd)); }
#else
int dup_fd(int fd) { return ::dup(fd); }
int dup2_fd(int from, int to) { return ::dup2(from, to); }
void close_fd(int fd) { static_cast<void>(::close(fd)); }
#endif

/// Points the file descriptor `fd` (1 or 2) at a file while it lives; text() restores it and
/// returns what was written there.
class FdToFile
{
public:
    FdToFile(int fd, const std::string& name) :
      fd_(fd),
      path_(std::filesystem::temp_directory_path() /
            ("vehicle_sil_ecu_main_" + name + "_" + std::to_string(std::random_device{}()) + ".txt"))
    {
        flush();
        file_ = std::fopen(path_.string().c_str(), "wb");
        CHECK(file_ != nullptr);
        saved_ = dup_fd(fd_);
        CHECK(saved_ != -1);
        CHECK(dup2_fd(fileno_of(file_), fd_) != -1);
    }
    ~FdToFile() { restore(); }
    FdToFile(const FdToFile&) = delete;
    FdToFile& operator=(const FdToFile&) = delete;

    std::string text()
    {
        restore();
        std::string out;
        {
            std::ifstream in(path_, std::ios::binary);
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        return out;
    }

private:
    static void flush()
    {
        std::cout.flush();
        std::cerr.flush();
        std::fflush(stdout);
        std::fflush(stderr);
    }
    static int fileno_of(std::FILE* f)
    {
#ifdef _WIN32
        return _fileno(f);
#else
        return ::fileno(f);
#endif
    }
    void restore()
    {
        if (saved_ == -1)
        {
            return;
        }
        flush();
        dup2_fd(saved_, fd_);
        close_fd(saved_);
        saved_ = -1;
        std::fclose(file_);
        file_ = nullptr;
    }

    int fd_;
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;
    int saved_ = -1;
};


const EcuMainOptions kOptions{.name = "Engine", .step = 10ms};

/// An ECU with a J1939 node and a variable; counts its steps; throws in the step at `fail_at`.
struct Probe
{
    int steps = 0;
    int made = 0;
    std::string made_as;
    std::optional<SimTime> fail_at;

    void set_up(Ecu& ecu)
    {
        ++made;
        made_as = ecu.describe().name;
        static_cast<void>(ecu.j1939("CAN").add_node({.address = 0x10}));
        ecu.output("steps", &steps);
        ecu.on_step([this](SimTime now) {
            ++steps;
            if (fail_at && now == *fail_at)
            {
                throw std::runtime_error("the model diverged");
            }
        });
    }
};

/// The description as ecu_main() writes it: the executable offers the ECU's variables over SIL
/// Kit (unless --no-control).
vehicle_sil::EcuDescription offered(vehicle_sil::EcuDescription description)
{
    description.control = vehicle_sil::ControlOffer{"silkit", 1};
    return description;
}

/// What one call of ecu_main() wrote and returned.
struct Result
{
    int code = -1;
    std::string out;
    std::string err;
};

/// Calls ecu_main(program, args...) with stdout and stderr captured.
Result call(const std::vector<std::string>& args, Probe& probe, const EcuMainOptions& options = kOptions)
{
    std::vector<const char*> argv{"/some/dir/engine_ecu"};
    for (const auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    // std::cout (the usage) and fd 1 (--describe's JSON) are both stdout.
    FdToFile fd1(1, "call_stdout");
    std::ostringstream out;
    std::ostringstream err;
    auto* oldOut = std::cout.rdbuf(out.rdbuf());
    auto* oldErr = std::cerr.rdbuf(err.rdbuf());
    Result r;
    try
    {
        r.code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), options,
                                       [&probe](Ecu& ecu) { probe.set_up(ecu); });
    }
    catch (...)
    {
        std::cout.rdbuf(oldOut);
        std::cerr.rdbuf(oldErr);
        throw;
    }
    std::cout.rdbuf(oldOut);
    std::cerr.rdbuf(oldErr);
    r.out = out.str() + fd1.text();
    r.err = err.str();
    return r;
}

Result call(const std::vector<std::string>& args)
{
    Probe probe;
    return call(args, probe);
}

std::size_t lines(const std::string& text)
{
    std::size_t n = 0;
    for (const char c : text)
    {
        n += c == '\n' ? 1 : 0;
    }
    return n;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void require(bool condition, const std::string& what, const Result& r)
{
    if (!condition)
    {
        throw vehicle_sil::unit::Failure(what + " (exit " + std::to_string(r.code) + ")\n--- stdout ---\n" + r.out +
                                         "--- stderr ---\n" + r.err);
    }
}

/// The first line of `text`, without its newline.
std::string first_line(const std::string& text) { return text.substr(0, text.find('\n')); }

} // namespace

TEST_CASE(ecu_main_without_arguments_writes_the_usage_and_returns_4)
{
    Probe probe;
    const Result r = call({}, probe);
    require(r.code == 4, "exit 4", r);
    require(r.out.empty(), "nothing on stdout", r);
    require(first_line(r.err) ==
                "error: Engine: no arguments: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>",
            "the error line", r);
    // The usage names the program (without its directory), the ECU and its step.
    require(contains(r.err, "\nusage: engine_ecu --describe [--name <name>] [--silkit-config <file>]\n"), "the usage", r);
    require(contains(r.err, "the ECU \"Engine\", step 10 ms\n"), "the ECU in the usage", r);
    CHECK_EQ(probe.made, 0);
}

TEST_CASE(ecu_main_help_writes_the_usage_to_stdout)
{
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--help"}, {"-h"}, {"--bogus", "--help"}, {"--name", "--help"}, {"--describe", "--name", "x", "-h"}})
    {
        Probe probe;
        const Result r = call(args, probe);
        require(r.code == 0, "exit 0", r);
        require(r.err.empty(), "nothing on stderr", r);
        require(r.out.rfind("usage: engine_ecu --describe", 0) == 0, "the usage on stdout", r);
        require(contains(r.out, "exit codes: 0 done, 1 the ECU failed in the run, 3 the set-up failed"), "exit codes", r);
        require(contains(r.out, " --describe [--name <name>] [--silkit-config <file>]\n") &&
                    contains(r.out, "  --silkit-config <file>  with --registry-uri: a SIL Kit participant configuration (YAML or JSON)\n"),
                "the usage has --silkit-config", r);
        CHECK_EQ(probe.made, 0);
    }
}

TEST_CASE(ecu_main_refuses_bad_command_lines)
{
    struct Case
    {
        std::vector<std::string> args;
        std::string error; ///< the error line after "error: Engine: "
    };
    const std::vector<Case> cases{
        {{"--bogus"}, "unknown argument \"--bogus\" (see the usage below)"},
        {{"describe"}, "unknown argument \"describe\" (see the usage below)"},
        {{"--name=Engine2", "--describe"}, "unknown argument \"--name=Engine2\" (write the value after a space: --name Engine2)"},
        {{"--describe", "--describe"}, "\"--describe\" is given twice; give it once"},
        {{"--describe", "--name", "A", "--name", "B"}, "\"--name\" is given twice; give it once"},
        {{"--describe", "--name"}, "--name needs a name after it (e.g. --name Engine2)"},
        {{"--name", "--describe"}, "--name needs a name after it (e.g. --name Engine2), not the option \"--describe\""},
        {{"--describe", "--name", ""}, "--name needs a name that is not empty (e.g. --name Engine2)"},
        {{"--registry-uri"}, "--registry-uri needs the registry's URI after it (e.g. --registry-uri silkit://localhost:8500)"},
        {{"--registry-uri", ""}, "--registry-uri needs a URI that is not empty (e.g. --registry-uri silkit://localhost:8500)"},
        {{"--name", "Engine2"}, "nothing to do: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>"},
        {{"--describe", "--local", "--run-for-ms", "10"}, "--describe, --registry-uri and --local exclude each other: give one of them"},
        {{"--registry-uri", "silkit://localhost:8500", "--local", "--run-for-ms", "10"},
         "--describe, --registry-uri and --local exclude each other: give one of them"},
        {{"--local"}, "--local needs --run-for-ms <ms>, how much virtual time to run (e.g. --local --run-for-ms 1000)"},
        {{"--describe", "--run-for-ms", "10"}, "--run-for-ms is for --local: --describe runs nothing"},
        {{"--registry-uri", "silkit://localhost:8500", "--run-for-ms", "10"},
         "--run-for-ms is for --local: with --registry-uri, the controller ends the run (vehicle_sil_sim_controller --stop-at-ms)"},
        {{"--local", "--run-for-ms", "0"},
         "--run-for-ms needs a whole number of milliseconds from 1 to 1000000000000, not \"0\" (e.g. --run-for-ms 1000)"},
        {{"--local", "--run-for-ms", "1.5"},
         "--run-for-ms needs a whole number of milliseconds from 1 to 1000000000000, not \"1.5\" (e.g. --run-for-ms 1000)"},
        {{"--local", "--run-for-ms", "-5"},
         "--run-for-ms needs a whole number of milliseconds from 1 to 1000000000000, not \"-5\" (e.g. --run-for-ms 1000)"},
        {{"--local", "--run-for-ms", "1000000000001"},
         "--run-for-ms needs a whole number of milliseconds from 1 to 1000000000000, not \"1000000000001\" (e.g. --run-for-ms 1000)"},
        {{"--describe", "--start-timeout-ms", "100"}, "--start-timeout-ms is for --registry-uri: --describe starts nothing to wait for"},
        {{"--local", "--run-for-ms", "10", "--start-timeout-ms", "100"},
         "--start-timeout-ms is for --registry-uri: --local starts nothing to wait for"},
        {{"--registry-uri", "silkit://localhost:8500", "--start-timeout-ms", "0"},
         "--start-timeout-ms needs a whole number of milliseconds from 1 to 1000000000000, not \"0\" (e.g. --start-timeout-ms 1000)"},
        {{"--registry-uri", "silkit://localhost:8500", "--start-timeout-ms"},
         "--start-timeout-ms needs a duration in milliseconds after it (e.g. --start-timeout-ms 60000)"},
        {{"--describe", "--name", "\x83G\x83\x93"},
         "--name needs ASCII or UTF-8 text, not the bytes 83 47 83 93 (a name in another encoding, e.g. a Windows code page)"},
        {{"--start-timeout-ms=5", "--describe"}, "unknown argument \"--start-timeout-ms=5\" (write the value after a space: --start-timeout-ms 5)"},
        {{"--describe", "--silkit-config"},
         "--silkit-config needs the path of a SIL Kit participant configuration file after it (e.g. --silkit-config "
         "engine.silkit.yaml)"},
        {{"--describe", "--silkit-config", ""},
         "--silkit-config needs a path that is not empty (e.g. --silkit-config engine.silkit.yaml)"},
        {{"--silkit-config=a.yaml", "--describe"},
         "unknown argument \"--silkit-config=a.yaml\" (write the value after a space: --silkit-config a.yaml)"},
        {{"--local", "--run-for-ms", "10", "--silkit-config", "a.yaml"},
         "--silkit-config is for --registry-uri: --local runs the ECU on local virtual buses, without SIL Kit; leave "
         "--silkit-config out"},
    };
    for (const auto& c : cases)
    {
        Probe probe;
        const Result r = call(c.args, probe);
        require(r.code == 4, "exit 4 for " + c.error, r);
        require(r.out.empty(), "nothing on stdout", r);
        require(first_line(r.err) == "error: Engine: " + c.error, "the error line " + c.error, r);
        require(contains(r.err, "\nusage: engine_ecu"), "the usage after the error", r);
        CHECK_EQ(probe.made, 0);
    }
}

TEST_CASE(ecu_main_takes_every_argument_the_launcher_gives)
{
    // vehicle_sil_launch refuses these in a command because it gives them itself
    // (ecu_executable::kLauncherArguments): ecu_main must know each one, with its value if it takes
    // one, and answer with its exit codes (ecu_executable).
    {
        // What ecu_main says of an argument it does not know, which the loop must not see.
        Probe probe;
        const Result r = call({"--bogus"}, probe);
        require(contains(r.err, "unknown argument \"--bogus\""), "the words for an unknown argument", r);
    }
    for (const std::string_view argument : vehicle_sil::ecu_executable::kLauncherArguments)
    {
        Probe probe;
        const std::string arg(argument);
        const Result r = call({arg}, probe);
        require(!contains(r.err, "unknown argument"), arg + " is an argument of ecu_main", r);
        // Alone: the usage (--help), the description (--describe) or what is missing with it.
        require(r.code == vehicle_sil::ecu_executable::kExitOk || r.code == vehicle_sil::ecu_executable::kExitUsage,
                arg + " alone: exit 0 or 4", r);
    }
}

TEST_CASE(ecu_main_describe_writes_the_json_of_the_ecu)
{
    // The same ECU set up on a Simulation of its own gives the expected text.
    Probe reference;
    vehicle_sil::Simulation sim;
    reference.set_up(sim.add_ecu("Engine", 10ms));
    const std::string expected = to_json(offered(sim.ecu("Engine").describe()));

    Probe probe;
    const Result r = call({"--describe"}, probe);
    require(r.code == 0, "exit 0", r);
    require(r.err.empty(), "nothing on stderr", r);
    require(r.out == expected, "the JSON of the ECU", r);
    CHECK_EQ(probe.made, 1);
    CHECK_EQ(probe.steps, 0); // nothing ran
    require(contains(r.out, "\"name\": \"Engine\",\n  \"step_ns\": 10000000,\n"), "name and step", r);
}

TEST_CASE(ecu_main_name_replaces_the_ecus_name)
{
    // "-h" as a value is the value, not --help.
    Probe dash;
    const Result h = call({"--describe", "--name", "-h"}, dash);
    require(h.code == 0 && contains(h.out, "\n  \"name\": \"-h\",\n"), "-h as the name", h);

    Probe probe;
    const Result r = call({"--name", "Engine2", "--describe"}, probe);
    require(r.code == 0, "exit 0", r);
    require(contains(r.out, "\n  \"name\": \"Engine2\",\n"), "the name in the JSON", r);
    CHECK(probe.made_as == "Engine2");

    Probe local;
    const Result l = call({"--local", "--run-for-ms", "20", "--name", "Engine3"}, local);
    require(l.code == 0, "exit 0", l);
    CHECK(local.made_as == "Engine3");

    // The name of a failure is the one given.
    const Result bad = call({"--name", "Engine4", "--bogus"});
    require(first_line(bad.err) == "error: Engine: unknown argument \"--bogus\" (see the usage below)",
            "a bad command line names the ECU of the executable", bad);
    Probe failing;
    failing.fail_at = SimTime{} + 20ms;
    const Result f = call({"--local", "--run-for-ms", "100", "--name", "Engine5"}, failing);
    require(f.err.rfind("error: Engine5: ", 0) == 0, "a failure names the ECU as named", f);
}

TEST_CASE(ecu_main_local_runs_the_ecu_alone)
{
    Probe probe;
    const Result r = call({"--local", "--run-for-ms", "100"}, probe);
    require(r.code == 0, "exit 0", r);
    require(r.out.empty() && r.err.empty(), "nothing written", r);
    CHECK_EQ(probe.made, 1);
    CHECK_EQ(probe.steps, 10); // 0, 10, ..., 90 ms
}

TEST_CASE(ecu_main_reports_a_failed_step_with_exit_1)
{
    Probe probe;
    probe.fail_at = SimTime{} + 30ms;
    const Result r = call({"--run-for-ms", "100", "--local"}, probe);
    require(r.code == 1, "exit 1", r);
    require(r.out.empty(), "nothing on stdout", r);
    require(lines(r.err) == 1, "one line", r);
    // The ECU named once, then why.
    require(r.err.rfind("error: Engine: ", 0) == 0 && !contains(r.err, "Engine: Engine:"), "the ECU named once", r);
    require(contains(r.err, "the model diverged"), "why", r);
}

TEST_CASE(ecu_main_reports_a_failed_set_up_with_exit_3)
{
    for (const auto& args : std::vector<std::vector<std::string>>{{"--describe"}, {"--local", "--run-for-ms", "10"}})
    {
        std::vector<const char*> argv{"ecu"};
        for (const auto& a : args)
        {
            argv.push_back(a.c_str());
        }
        FdToFile fd1(1, "set_up_stdout");
        std::ostringstream err;
        std::ostringstream out;
        auto* oldErr = std::cerr.rdbuf(err.rdbuf());
        auto* oldOut = std::cout.rdbuf(out.rdbuf());
        const int code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), kOptions, [](Ecu& ecu) {
            static_cast<void>(ecu.j1939("CAN"));
            throw std::runtime_error("no calibration file\nfound");
        });
        std::cerr.rdbuf(oldErr);
        std::cout.rdbuf(oldOut);
        const Result r{code, out.str() + fd1.text(), err.str()};
        require(code == 3, "exit 3", r);
        require(r.out.empty(), "nothing on stdout", r);
        require(r.err == "error: Engine: no calibration file found\n", "one line with why", r);
    }

    // A bad step from the executable's options is a set-up failure too.
    Probe probe;
    const Result r = call({"--describe"}, probe, {.name = "Engine", .step = vehicle_sil::SimDuration::zero()});
    require(r.code == 3, "exit 3 for step 0", r);
    require(r.err.rfind("error: Engine: ", 0) == 0 && contains(r.err, "step"), "names the step", r);
}

TEST_CASE(ecu_main_reports_a_registry_it_cannot_join_with_exit_3)
{
    Probe probe;
    const Result r = call({"--registry-uri", "not-a-uri"}, probe);
    require(r.code == 3, "exit 3", r);
    require(lines(r.err) == 1, "one line (SIL Kit's message has line breaks)", r);
    require(r.err.rfind("error: Engine: cannot join the simulation at \"not-a-uri\": ", 0) == 0, "the URI", r);
    require(contains(r.err, "(check that vehicle_sil_launch run (or the registry) is started first and waits for the "
                            "participants; that the URI has the address of the computer it runs on (127.0.0.1 on this "
                            "one) and the port of its registry_uri, e.g. silkit://192.0.2.10:8500; that a firewall "
                            "there lets this computer in; and that no other participant there is named \"Engine\")\n"),
            "what to do", r);
    CHECK_EQ(probe.made, 0);
}

TEST_CASE(ecu_main_says_only_the_name_when_the_registry_refuses_it)
{
    // The registry was reached: what to check for a registry that cannot be reached is left out.
    vehicle_sil::unit::InProcessRegistry registry;
    vehicle_sil::ParticipantRuntimeOptions taken;
    taken.participant_name = "Engine";
    taken.registry_uri = registry.uri;
    const vehicle_sil::ParticipantRuntime first(taken);
    Probe probe;
    const Result r = call({"--registry-uri", registry.uri, "--start-timeout-ms", "300"}, probe);
    require(r.code == 3, "exit 3", r);
    require(lines(r.err) == 1, "one line", r);
    require(contains(r.err, "(the registry refused the name \"Engine\" for 300 ms: another participant of this simulation "
                            "uses it"),
            "the name, and how long (no longer than --start-timeout-ms)", r);
    require(!contains(r.err, "(check that"), "nothing of a registry that cannot be reached", r);
    CHECK_EQ(probe.made, 0);
}

namespace {

/// An application that takes arguments after the Ecu&: one by reference, one move-only.
class WithArgs
{
public:
    WithArgs(Ecu& ecu, int& made, std::unique_ptr<int> value)
    {
        ++made;
        static_cast<void>(ecu.can(*value == 7 ? "SEVEN" : "OTHER"));
    }
};

} // namespace

TEST_CASE(ecu_main_template_passes_its_arguments_to_the_application)
{
    std::vector<std::string> args{"ecu", "--describe", "--name", "Templated"};
    std::vector<char*> argv;
    for (auto& a : args)
    {
        argv.push_back(a.data());
    }
    FdToFile fd1(1, "template_stdout");
    int made = 0;
    const int code = vehicle_sil::ecu_main<WithArgs>(static_cast<int>(argv.size()), argv.data(),
                                                     {.name = "Engine", .step = 1ms}, made, std::make_unique<int>(7));
    const std::string out = fd1.text();
    CHECK_EQ(code, 0);
    CHECK_EQ(made, 1);
    CHECK(contains(out, "\"name\": \"Templated\""));
    CHECK(contains(out, "\"name\": \"SEVEN\""));
}

namespace {

/// An ECU with one J1939 node and nothing that runs.
void node_only(Ecu& ecu) { static_cast<void>(ecu.j1939("CAN").add_node({.address = 0x10})); }

/// argv for `args` (which must outlive it).
std::vector<const char*> argv_of(const std::vector<std::string>& args)
{
    std::vector<const char*> argv{"ecu"};
    for (const auto& a : args)
    {
        argv.push_back(a.c_str());
    }
    return argv;
}

/// Calls ecu_main() with `set_up` and stderr captured (through std::cerr; stdout as it is).
Result call_with(const std::vector<std::string>& args, const std::function<void(Ecu&)>& set_up,
                 const EcuMainOptions& options = kOptions)
{
    const auto argv = argv_of(args);
    FdToFile fd1(1, "call_with_stdout");
    std::ostringstream err;
    auto* oldErr = std::cerr.rdbuf(err.rdbuf());
    std::ostringstream out;
    auto* oldOut = std::cout.rdbuf(out.rdbuf());
    Result r;
    try
    {
        r.code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), options, set_up);
    }
    catch (...)
    {
        std::cerr.rdbuf(oldErr);
        std::cout.rdbuf(oldOut);
        throw;
    }
    std::cerr.rdbuf(oldErr);
    std::cout.rdbuf(oldOut);
    r.out = out.str() + fd1.text();
    r.err = err.str();
    return r;
}

} // namespace

namespace {

/// Writes to stdout from its constructor and its destructor.
class Noisy
{
public:
    explicit Noisy(Ecu& ecu)
    {
        node_only(ecu);
        std::printf("Engine ready (printf)\n");
        std::cout << "Engine ready (cout)" << std::endl;
    }
    ~Noisy()
    {
        std::printf("Engine stopped (destructor)\n");
        std::fflush(stdout);
    }
    Noisy(const Noisy&) = delete;
    Noisy& operator=(const Noisy&) = delete;
};

} // namespace

TEST_CASE(ecu_main_describe_keeps_stdout_for_the_json)
{
    // The application writes to stdout through C stdio and C++, in its constructor and in its
    // destructor (after the JSON). With --describe all of it goes to stderr, and stdout (fd 1)
    // carries the JSON only; also what is written after ecu_main() returned.
    vehicle_sil::Simulation sim;
    node_only(sim.add_ecu("Engine", 10ms));
    const std::string expected = to_json(offered(sim.ecu("Engine").describe()));

    const std::vector<std::string> args{"--describe"};
    const auto argv = argv_of(args); // points into args
    std::string out;
    std::string err;
    int code = -1;
    {
        FdToFile capturedErr(2, "stderr");
        FdToFile capturedOut(1, "stdout");
        code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), kOptions,
                                     [](Ecu& ecu) { ecu.emplace<Noisy>(ecu); });
        std::printf("after ecu_main\n"); // as atexit or a thread of the application would
        std::fflush(stdout);
        out = capturedOut.text();
        err = capturedErr.text();
    }
    CHECK_EQ(code, 0);
    if (out != expected)
    {
        throw vehicle_sil::unit::Failure("stdout is not the JSON:\n" + out);
    }
    CHECK(contains(err, "Engine ready (printf)\n"));
    CHECK(contains(err, "Engine ready (cout)\n"));
    CHECK(contains(err, "Engine stopped (destructor)\n"));
    CHECK(contains(err, "after ecu_main\n"));
}

namespace {

/// A new directory with SIL Kit participant configuration files for one test, removed with it.
struct ConfigurationFiles
{
    ConfigurationFiles() :
      directory(std::filesystem::temp_directory_path() /
                ("vsil-ecu-main-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
    {
        std::filesystem::create_directories(directory);
    }
    ~ConfigurationFiles()
    {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    /// Writes `text` to `name` there; returns the file's path.
    std::string write(const std::string& name, const std::string& text) const
    {
        std::ofstream out(directory / name, std::ios::binary);
        out << text;
        CHECK(out.good());
        return (directory / name).string();
    }
    std::filesystem::path directory;
};

} // namespace

TEST_CASE(ecu_main_describe_checks_the_silkit_configuration_and_keeps_stdout_for_the_json)
{
    // vehicle_sil_launch check gives --describe the arguments of the participant's command: the
    // configuration file is read and checked, nothing connects, and stdout carries the JSON only
    // (SIL Kit writes its warning about a key it does not know to stdout).
    vehicle_sil::Simulation sim;
    node_only(sim.add_ecu("Engine", 10ms));
    const std::string expected = to_json(offered(sim.ecu("Engine").describe()));
    const ConfigurationFiles files;
    const std::string file = files.write("engine.silkit.yaml", "NotAKeyOfSilKit: 1\nMiddleware:\n  ConnectAttempts: 3\n");

    const std::vector<std::string> args{"--describe", "--silkit-config", file};
    const auto argv = argv_of(args);
    std::string out;
    std::string err;
    int code = -1;
    {
        FdToFile capturedErr(2, "stderr");
        FdToFile capturedOut(1, "stdout");
        code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), kOptions, [](Ecu& ecu) { node_only(ecu); });
        std::cout.flush();
        out = capturedOut.text();
        err = capturedErr.text();
    }
    CHECK_EQ(code, 0);
    if (out != expected)
    {
        throw vehicle_sil::unit::Failure("stdout is not the JSON:\n" + out);
    }
    CHECK(contains(err, "NotAKeyOfSilKit")); // SIL Kit's warning

    // What the run would refuse is said now, with the option and the file, and exit code 3.
    const std::string missing = (files.directory / "none.silkit.yaml").string();
    const Result none = call({"--describe", "--silkit-config", missing});
    require(none.code == 3 && none.out.empty() && lines(none.err) == 1, "exit 3, one line", none);
    require(none.err == "error: Engine: --silkit-config \"" + missing + "\": there is no SIL Kit participant configuration file \"" +
                            missing + "\": give the path of a YAML or JSON file that exists (a relative path is taken from "
                            "the working directory of the process)\n",
            "the missing file", none);

    const std::string named = files.write("named.silkit.yaml", "ParticipantName: Motor\n");
    const Result other = call({"--describe", "--silkit-config", named});
    require(other.code == 3 && other.out.empty(), "exit 3", other);
    require(other.err.rfind("error: Engine: --silkit-config \"" + named + "\": the SIL Kit configuration of the participant "
                            "\"Engine\" sets ParticipantName to \"Motor\": ", 0) == 0 &&
                contains(other.err, "or set it to \"Engine\"\n"),
            "the other name", other);
    require(call({"--describe", "--silkit-config", named, "--name", "Motor"}).code == 0, "the same name passes",
            call({"--describe", "--silkit-config", named, "--name", "Motor"}));

    const std::string broken = files.write("broken.silkit.yaml", "Middleware:\n  ConnectAttempts: many\n");
    const Result refused = call({"--describe", "--silkit-config", broken});
    require(refused.code == 3 && refused.out.empty() && lines(refused.err) >= 1, "exit 3", refused);
    require(refused.err.rfind("error: Engine: --silkit-config \"" + broken + "\": SIL Kit does not accept the configuration: ", 0) == 0 &&
                contains(refused.err, "(correct the file, or a file it includes; SIL Kit's documentation of the participant "
                                      "configuration lists the keys and their values)\n"),
            "SIL Kit's refusal", refused);

    // A file that is not YAML: SIL Kit's refusal as well, with the place in the text.
    const std::string not_yaml = files.write("not-yaml.silkit.yaml", "Description: x\nLogging: [not, a, map\n");
    const Result unread = call({"--describe", "--silkit-config", not_yaml});
    require(unread.code == 3 && unread.out.empty(), "exit 3", unread);
    require(unread.err.rfind("error: Engine: --silkit-config \"" + not_yaml + "\": SIL Kit does not accept the configuration: ", 0) == 0 &&
                contains(unread.err, "(at line 3, column 1 of the configuration text) ") && contains(unread.err, "(correct the file, or a file it includes;"),
            "SIL Kit's refusal of a text that is not YAML", unread);

    // A member with nothing under it is fine, and nothing else is written to stderr (the YAML
    // library's own error handler would write a line there).
    const std::string empty_member = files.write("empty-member.silkit.yaml", "Experimental:\n  TimeSynchronization:\n    # AnimationFactor: 2\n");
    const Result accepted = call({"--describe", "--silkit-config", empty_member});
    require(accepted.code == 0 && accepted.err.empty(), "exit 0, nothing on stderr", accepted);

    // A URI in the file is not compared by --describe (it knows none); --registry-uri compares it,
    // before it connects (nothing listens at this URI: the error would be another one).
    const std::string elsewhere = files.write("uri.silkit.yaml", "Middleware:\n  RegistryUri: silkit://192.0.2.10:8500\n");
    require(call({"--describe", "--silkit-config", elsewhere}).code == 0, "--describe does not compare the URI",
            call({"--describe", "--silkit-config", elsewhere}));
    const Result uri = call({"--registry-uri", "silkit://127.0.0.1:9", "--silkit-config", elsewhere});
    require(uri.code == 3 && lines(uri.err) == 1, "exit 3, one line", uri);
    require(uri.err.rfind("error: Engine: --silkit-config \"" + elsewhere + "\": the SIL Kit configuration of the participant "
                          "\"Engine\" sets Middleware.RegistryUri to \"silkit://192.0.2.10:8500\", but the participant is told "
                          "to join \"silkit://127.0.0.1:9\": ", 0) == 0,
            "the other URI", uri);
    const std::string dynamic =
        files.write("dynamic.silkit.yaml", "Experimental:\n  TimeSynchronization:\n    DynamicSimulationStep: true\n");
    const Result steps = call({"--registry-uri", "silkit://127.0.0.1:9", "--silkit-config", dynamic});
    require(steps.code == 3 && contains(steps.err, "DynamicSimulationStep to true: "), "dynamic steps", steps);
}

TEST_CASE(ecu_main_gives_the_silkit_configuration_to_its_participant)
{
    // A file sink for SIL Kit's log, in a file that includes another one beside it: the
    // participant writes its log there (here it joins a registry without a controller, and gives up).
    vehicle_sil::Simulation registry(vehicle_sil::SilKitOptions{});
    const ConfigurationFiles files;
    const std::string log_name = (files.directory / "engine_log").generic_string();
    files.write("logging.silkit.yaml", "Logging:\n  Sinks:\n    - Type: File\n      Level: Debug\n      LogName: '" + log_name + "'\n");
    const std::string file = files.write("engine.silkit.yaml", "Includes:\n  Files: [logging.silkit.yaml]\nMiddleware:\n  ConnectAttempts: 2\n");
    const Result r = call_with({"--registry-uri", registry.registry_uri(), "--start-timeout-ms", "300", "--silkit-config", file},
                               [](Ecu& ecu) { node_only(ecu); });
    require(r.code == 3 && r.err.rfind("error: Engine: the simulation did not start: ", 0) == 0, "it joined and gave up", r);
    std::size_t logs = 0;
    for (const auto& entry : std::filesystem::directory_iterator(files.directory))
    {
        const std::string name = entry.path().filename().string();
        if (name.rfind("engine_log", 0) == 0)
        {
            ++logs;
            CHECK(entry.file_size() > 0);
        }
    }
    CHECK_EQ(logs, std::size_t{1});
}

TEST_CASE(ecu_main_describe_with_stderr_closed_keeps_stdout_for_the_json)
{
    // stderr closed: what the application writes to stdout or stderr goes nowhere (the null
    // device), not into stdout before the JSON.
    vehicle_sil::Simulation sim;
    node_only(sim.add_ecu("Engine", 10ms));
    const std::string expected = to_json(offered(sim.ecu("Engine").describe()));

    const std::vector<std::string> args{"--describe"};
    const auto argv = argv_of(args);
    std::string out;
    int code = -1;
    {
        FdToFile capturedOut(1, "stdout_no_stderr");
        std::cerr.flush();
        std::fflush(stderr);
        const int savedErr = dup_fd(2);
        CHECK(savedErr != -1);
        close_fd(2);
        code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), kOptions, [](Ecu& ecu) {
            ecu.emplace<Noisy>(ecu);
            std::fprintf(stderr, "to stderr\n");
        });
        std::fflush(stdout);
        std::fflush(stderr);
        dup2_fd(savedErr, 2);
        close_fd(savedErr);
        std::clearerr(stderr);
        std::cerr.clear();
        out = capturedOut.text();
    }
    CHECK_EQ(code, 0);
    if (out != expected)
    {
        throw vehicle_sil::unit::Failure("stdout is not the JSON:\n" + out);
    }
}

TEST_CASE(ecu_main_reports_a_closed_stdout_with_exit_3)
{
    // On Windows this runs through the UCRT's checks of a closed descriptor, which ecu_main()
    // turns into errors on its thread instead of ending the process.
    const std::vector<std::string> args{"--describe"};
    const auto argv = argv_of(args);
    std::ostringstream err;
    std::cout.flush();
    std::fflush(stdout);
    const int saved = dup_fd(1);
    CHECK(saved != -1);
    close_fd(1);
    auto* oldErr = std::cerr.rdbuf(err.rdbuf());
    const int code = vehicle_sil::ecu_main(static_cast<int>(argv.size()), argv.data(), kOptions,
                                           [](Ecu& ecu) { node_only(ecu); });
    std::cerr.rdbuf(oldErr);
    std::fflush(stdout);
    dup2_fd(saved, 1);
    close_fd(saved);
    std::cout.clear();
    std::clearerr(stdout);
    CHECK_EQ(code, 3);
    CHECK(err.str() == "error: Engine: could not write the description to stdout (is it closed?)\n");
}

TEST_CASE(ecu_main_reports_other_set_up_failures_with_exit_3)
{
    // An exception that is not a std::exception.
    const Result thrown = call_with({"--local", "--run-for-ms", "10"}, [](Ecu&) { throw 42; });
    require(thrown.code == 3, "exit 3", thrown);
    require(thrown.err == "error: Engine: the set-up threw an exception that is not a std::exception\n", "the line", thrown);

    // A variable name that is reserved for a Peer's monitoring, found already by --describe.
    const Result reserved = call_with({"--describe"}, [](Ecu& ecu) {
        static int x = 0;
        ecu.output("peer.steps", &x);
    });
    require(reserved.code == 3 && reserved.out.empty(), "exit 3, no JSON", reserved);
    require(contains(reserved.err, "the variable name \"peer.steps\" starts with \"peer.\", which is reserved"),
            "names the variable and the rule", reserved);

    // An ECU name in the executable that is not UTF-8.
    const Result encoding = call_with({"--describe"}, [](Ecu&) {}, {.name = "\x83G", .step = 1ms});
    require(encoding.code == 3, "exit 3", encoding);
    require(contains(encoding.err, "EcuMainOptions::name is not ASCII or UTF-8 text (the bytes 83 47)"), "the bytes", encoding);
}

TEST_CASE(ecu_main_reports_a_registry_where_nothing_listens_with_exit_3)
{
    const Result r = call_with({"--registry-uri", "silkit://127.0.0.1:9"}, [](Ecu& ecu) { node_only(ecu); });
    require(r.code == 3, "exit 3", r);
    require(lines(r.err) == 1, "one line", r);
    require(r.err.rfind("error: Engine: cannot join the simulation at \"silkit://127.0.0.1:9\": ", 0) == 0, "the URI", r);
    require(contains(r.err, "and that no other participant there is named \"Engine\")\n"), "the causes", r);
}

TEST_CASE(ecu_main_start_timeout_and_start_failure)
{
    // A registry of this process and no controller: the start times out after
    // --start-timeout-ms, with the hint about the controller.
    vehicle_sil::Simulation registry(vehicle_sil::SilKitOptions{});
    const std::string uri = registry.registry_uri();
    const auto started = std::chrono::steady_clock::now();
    const Result timeout = call_with({"--registry-uri", uri, "--start-timeout-ms", "300", "--name", "Late"},
                                     [](Ecu& ecu) { node_only(ecu); });
    require(timeout.code == 3, "exit 3", timeout);
    require(std::chrono::steady_clock::now() - started < 20s, "it waited about 300 ms (and the shutdown)", timeout);
    require(timeout.err.rfind("error: Late: the simulation did not start: ", 0) == 0, "the start", timeout);
    require(contains(timeout.err, "within 300 ms"), "the timeout given", timeout);
    require(contains(timeout.err, "To wait longer: --start-timeout-ms)\n"), "the hint", timeout);

    // A start that fails for another reason (a variable that clashes with a network's
    // monitoring output on SIL Kit): no hint about a controller.
    const Result clash = call_with({"--registry-uri", uri, "--name", "Clash"}, [](Ecu& ecu) {
        static double x = 0;
        static_cast<void>(ecu.j1939("CAN").add_node({.address = 0x10}));
        ecu.input("j1939.CAN.bus_state", &x);
    });
    require(clash.code == 3, "exit 3", clash);
    require(clash.err.rfind("error: Clash: could not start: ", 0) == 0, "could not start", clash);
    require(!contains(clash.err, "controller"), "no hint about a controller", clash);
}

TEST_CASE(ecu_main_offers_its_variables_to_the_simulation_unless_no_control)
{
    // The other side: a controller with a hub on its participant, as vehicle_sil_launch has.
    const auto run_with = [](const std::string& name, const std::vector<std::string>& extra, vehicle_sil::control::Hub& hub) {
        vehicle_sil::Simulation registry(vehicle_sil::SilKitOptions{});
        vehicle_sil::SimulationControllerOptions c;
        c.registry_uri = registry.registry_uri();
        c.required_participants = {name};
        c.stop_at = 100ms;
        c.time_control = vehicle_sil::TimeControlOptions{};
        vehicle_sil::SimulationController controller(std::move(c));
        vehicle_sil::control_silkit::HubTransport transport(hub, controller.runtime());
        std::string start_error;
        std::thread starter([&] {
            try
            {
                controller.start(60s);
            }
            catch (const std::exception& e)
            {
                start_error = e.what();
            }
        });
        std::vector<std::string> args{"--registry-uri", registry.registry_uri(), "--name", name};
        args.insert(args.end(), extra.begin(), extra.end());
        Probe probe;
        // Plenty of time for the hub's answer to the last word: the hub is there and answers, so
        // the ECU does not wait long, however busy the machine is.
        const Result r = call(args, probe, EcuMainOptions{.name = "Engine", .step = 10ms, .last_word_wait = 60s});
        starter.join();
        require(r.code == 0, "the ECU ran to the end of the simulation", r);
        require(start_error.empty(), "the controller started: " + start_error, r);
        require(probe.steps >= 10, "the ECU stepped", r);
        // The ECU waited for the hub's answer to its last word before it let its participant go:
        // the hub has ended the connection. (An ECU with --no-control says none: nothing to wait for.)
        const auto deadline = std::chrono::steady_clock::now() + scaled_timeout(20s);
        while (std::chrono::steady_clock::now() < deadline)
        {
            const auto peers = hub.peers();
            if (peers.empty() || !peers.front().connected)
            {
                break;
            }
            std::this_thread::sleep_for(5ms);
        }
        return r;
    };
    {
        vehicle_sil::control::Hub hub;
        run_with("Offer", {}, hub);
        const auto peers = hub.peers();
        CHECK_EQ(peers.size(), std::size_t{1});
        CHECK(peers.at(0).participant == "Offer");
        CHECK(!peers.at(0).connected); // it said its last word when it ended
        CHECK(peers.at(0).schema.at(0).name == "ecu.connected"); // every ECU has it, first
        CHECK(peers.at(0).schema.at(1).name == "steps");
        const auto latest = hub.latest("Offer");
        CHECK(latest != nullptr);
        // The step is 10 ms, as is the sample period: the last sample before the stop at 100 ms.
        CHECK(latest->time >= SimTime{} + 90ms);
        CHECK(std::get<std::int64_t>(latest->values.at(1)) >= 10);
    }
    {
        vehicle_sil::control::Hub hub;
        run_with("Hidden", {"--no-control"}, hub);
        CHECK(hub.peers().empty());
    }
    // --no-control goes with every mode (a launcher passes a participant's arguments to
    // --describe too), and once.
    // --describe says whether the executable offers the variables: not with --no-control.
    const Result described = call({"--describe", "--no-control"});
    require(described.code == 0 && contains(described.out, "\"variables\"") && contains(described.out, "  \"control\": null\n"),
            "--describe --no-control", described);
    const Result offering = call({"--describe"});
    require(offering.code == 0 &&
                contains(offering.out, "  \"control\": {\n    \"transport\": \"silkit\",\n    \"wire_version\": 1\n  }\n"),
            "--describe says that the variables are offered", offering);
    const Result local = call({"--local", "--run-for-ms", "20", "--no-control"});
    require(local.code == 0, "--local --no-control", local);
    const Result twice = call({"--describe", "--no-control", "--no-control"});
    require(twice.code == 4 && contains(twice.err, "\"--no-control\" is given twice"), "--no-control twice", twice);
    const Result help = call({"--help"});
    require(contains(help.out, "[--no-control]") && contains(help.out, "  --no-control            with --registry-uri: do not offer"),
            "the usage has --no-control", help);
}
