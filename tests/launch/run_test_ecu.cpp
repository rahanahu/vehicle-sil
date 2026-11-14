// SPDX-License-Identifier: MIT
//
// An ecu_main executable for the tests of vehicle_sil_launch run, without networks. With
// "--autonomous <name> <registry URI>" it is instead a participant that runs on its own (autonomous,
// in real time, as a hand-started peer outside the file): it writes "<name>.running" in its working
// directory once it runs and stays until it is ended.
// It steps every
// 10 ms, or every <n> ms if the file "test-ecu-step-ms" in its working directory holds <n> (every
// test ECU started there, and its --describe). What it does besides comes from the file
// "<name>.behaviour" in its working directory (none: nothing), words separated by spaces:
//   pid                     writes its process ID to "<name>.pid" in its first step
//   exit-at-ms <t> <code>   ends the process at once with <code> in its step at <t> ms
//   throw-at-ms <t>         its step at <t> ms throws (the ECU fails)
//   hang-at-ms <t>          its step at <t> ms never returns
//   slow-ms <t>             each step at a multiple of 10 ms takes <t> ms of wall-clock time (sleeps)
//   slow-at-ms <t> <d>      its step at <t> ms takes <d> ms of wall-clock time (sleeps)
//   exit-after-wall-ms <t> <code>  ends the process with <code> <t> ms of wall-clock time after its
//                           first step, whatever the simulation does (also while it stands still)
//   chatter <n>             writes <n> lines of about 100 bytes to stdout in every step, and flushes them
//   linger-ms <t>           after the run, ends only <t> ms later (its process outlives its part in the
//                           simulation by that long)
//   print <word>            writes <word> and a line end to stdout with printf() when it is set up,
//                           without flushing (for --describe too, where it goes to stderr)
//   hang-after-end          after the run, does not end (its destructor writes its process ID to
//                           "<name>.hanging" and waits for ever), until:
//   stop-exit <code>          a stop request (POSIX SIGTERM, Windows Ctrl+Break) ends it with <code>
//   ignore-stop               a stop request is ignored
//                           main() installs the handler before ecu_main, which handles stop requests
//                           itself during the run and puts the handler back after it: no stop request
//                           after the run meets the default effect. One that comes before
//                           "<name>.hanging" is written may still reach ecu_main and be spent on the
//                           run that has stopped, so a test that stops the ECU waits for the file.
//   grandchild <helper>     in its first step, starts <helper> (tests/process/process_helper.cpp)
//                           as a process that sleeps, writes "<name>-grandchild.pid", and is not
//                           ended with the ECU; the step waits until the file is written
//   variables               declares the input "level", the parameter "gain", and the outputs
//                           "gain_at_0" (the parameter as the step at 0 saw it) and "ticks" (the
//                           number of its 10 ms steps so far)
//   scenario-variables      declares the input "level" (0 to 100), the enum input "mode" (off 0, on 1,
//                           auto 2), the parameter "gain", and the outputs "gain_at_0", "ticks",
//                           "level_changed_at_ms" (the start of the 10 ms step that first saw the
//                           level it has now; -1 before a change), "mode_now" (the mode, as an
//                           enum) and "pulse" (true in the step at 200 ms only). A level of 13
//                           is refused (the setter throws)
// With the file "test-ecu-no-control" in its working directory, the ECUs started there run with
// --no-control (their --describe still has the variables): an executable that describes
// variables and never offers them.
#include "spawn_sleeper.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>
#include <cstdint>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

// What a stop request does: kStopDefault its default effect, -1 nothing (ignore-stop), else end
// the process with this code (stop-exit). A hang-after-end ECU sets it in its first step.
constexpr int kStopDefault = -2;
volatile std::sig_atomic_t g_stop_code = kStopDefault;

#ifdef _WIN32
BOOL WINAPI on_stop(DWORD event)
{
    const int code = g_stop_code;
    if ((event != CTRL_BREAK_EVENT && event != CTRL_C_EVENT) || code == kStopDefault)
    {
        return FALSE;
    }
    if (code >= 0)
    {
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    }
    return TRUE;
}
#else
extern "C" void on_stop(int signal)
{
    const int code = g_stop_code;
    if (code >= 0)
    {
        ::_exit(code);
    }
    if (code == kStopDefault)
    {
        std::signal(signal, SIG_DFL);
        std::raise(signal);
    }
}
#endif

class TestEcu
{
public:
    explicit TestEcu(vehicle_sil::Ecu& ecu) : name_(ecu.describe().name)
    {
        std::ifstream in(name_ + ".behaviour");
        std::string word;
        while (in >> word)
        {
            if (word == "pid")
            {
                pid_ = true;
            }
            else if (word == "exit-at-ms")
            {
                in >> exit_at_ >> exit_code_;
            }
            else if (word == "throw-at-ms")
            {
                in >> throw_at_;
            }
            else if (word == "hang-at-ms")
            {
                in >> hang_at_;
            }
            else if (word == "slow-ms")
            {
                in >> slow_;
            }
            else if (word == "slow-at-ms")
            {
                in >> slow_at_ >> slow_at_for_;
            }
            else if (word == "exit-after-wall-ms")
            {
                in >> exit_after_ >> exit_after_code_;
            }
            else if (word == "print")
            {
                std::string text;
                in >> text;
                std::printf("%s\n", text.c_str()); // no flush: stdout is a pipe, so it stays in the buffer
            }
            else if (word == "chatter")
            {
                in >> chatter_;
            }
            else if (word == "linger-ms")
            {
                in >> linger_;
            }
            else if (word == "hang-after-end")
            {
                hang_ = true;
            }
            else if (word == "stop-exit")
            {
                in >> stop_code_;
            }
            else if (word == "ignore-stop")
            {
                stop_code_ = -1;
            }
            else if (word == "grandchild")
            {
                in >> grandchild_;
            }
            else if (word == "variables")
            {
                ecu.input("level", &level_);
                ecu.input("gain", &gain_, {.parameter = true});
                ecu.output("gain_at_0", &gain_at_0_);
                ecu.output("ticks", &ticks_);
            }
            else if (word == "scenario-variables")
            {
                const std::vector<vehicle_sil::control::EnumItem> modes{{"off", 0}, {"on", 1}, {"auto", 2}};
                ecu.agent().input(
                    "level", [this] { return level_; },
                    [this](std::int64_t v) {
                        if (v == 13)
                        {
                            throw std::runtime_error("13 is not a level");
                        }
                        level_ = v;
                    },
                    {.min = 0.0, .max = 100.0});
                ecu.agent().input_enum("mode", &mode_, modes);
                ecu.input("gain", &gain_, {.parameter = true});
                ecu.output("gain_at_0", &gain_at_0_);
                ecu.output("ticks", &ticks_);
                ecu.output("level_changed_at_ms", &level_changed_at_);
                ecu.agent().output_enum("mode_now", [this] { return mode_; }, modes);
                ecu.output("pulse", &pulse_);
            }
        }
        // Not in the constructor: --describe constructs the ECU too, and must start nothing.
        ecu.at(vehicle_sil::SimTime{}, [this](vehicle_sil::SimTime) {
            ran_ = true;
            if (hang_)
            {
                g_stop_code = stop_code_;
            }
            gain_at_0_ = gain_;
            if (pid_)
            {
                std::ofstream(name_ + ".pid") <<
#ifdef _WIN32
                    GetCurrentProcessId();
#else
                    ::getpid();
#endif
            }
            if (!grandchild_.empty())
            {
                vehicle_sil::test::spawn_sleeper(grandchild_, name_ + "-grandchild.pid");
                // Before the run goes on: as fast as possible, the simulation (and with it this
                // process) may end before a slow start of the new process got to write its file.
                if (!vehicle_sil::test::pid_written(name_ + "-grandchild.pid", std::chrono::seconds(30)))
                {
                    std::fprintf(stderr, "test ECU: the process it started did not write %s-grandchild.pid within 30 s\n",
                                 name_.c_str());
                }
            }
            if (exit_after_ >= 0)
            {
                std::thread([after = exit_after_, code = exit_after_code_] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(after));
                    std::_Exit(code);
                }).detach();
            }
        });
        ecu.every(10ms, [this](vehicle_sil::SimTime now) {
            ++ticks_;
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
            if (level_ != last_level_)
            {
                last_level_ = level_;
                level_changed_at_ = ms;
            }
            pulse_ = ms == 200;
            if (slow_ > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(slow_));
            }
            if (ms == slow_at_)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(slow_at_for_));
            }
            for (int i = 0; i < chatter_; ++i)
            {
                std::printf("chatter %lld.%d %s\n", ms, i, std::string(80, 'x').c_str());
            }
            if (chatter_ > 0)
            {
                std::fflush(stdout);
            }
            if (ms == exit_at_)
            {
                std::_Exit(exit_code_);
            }
            while (ms == hang_at_)
            {
                std::this_thread::sleep_for(1s);
            }
            if (ms == throw_at_)
            {
                throw std::runtime_error("the test ECU fails at " + std::to_string(ms) + " ms, as its behaviour says");
            }
        });
    }
    TestEcu(const TestEcu&) = delete;
    TestEcu& operator=(const TestEcu&) = delete;

    ~TestEcu()
    {
        if (ran_ && linger_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(linger_));
        }
        if (!hang_ || !ran_)
        {
            return; // also --describe constructs and destroys it
        }
        std::ofstream(name_ + ".hanging") <<
#ifdef _WIN32
            GetCurrentProcessId();
#else
            ::getpid();
#endif
        while (true)
        {
            std::this_thread::sleep_for(1s);
        }
    }

private:
    std::string name_;
    bool pid_ = false;
    std::string grandchild_;
    long long exit_at_ = -1;
    int exit_code_ = 0;
    long long throw_at_ = -1;
    long long hang_at_ = -1;
    long long slow_ = 0;
    long long slow_at_ = -1;
    long long slow_at_for_ = 0;
    long long exit_after_ = -1;
    int exit_after_code_ = 0;
    bool hang_ = false;
    int chatter_ = 0;
    long long linger_ = 0;
    bool ran_ = false;
    int stop_code_ = -1;
    std::int64_t level_ = 0;
    std::int64_t gain_ = 0;
    std::int64_t gain_at_0_ = -1;
    std::int64_t ticks_ = 0;
    std::int64_t last_level_ = 0;
    std::int64_t level_changed_at_ = -1;
    std::int64_t mode_ = 0;
    bool pulse_ = false;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc == 4 && std::string(argv[1]) == "--autonomous")
    {
        vehicle_sil::ParticipantRuntimeOptions options;
        options.participant_name = argv[2];
        options.registry_uri = argv[3];
        options.lifecycle_mode = vehicle_sil::LifecycleMode::Autonomous;
        vehicle_sil::ParticipantRuntime runtime(options);
        runtime.start(std::chrono::seconds(30));
        std::ofstream(std::string(argv[2]) + ".running") << "running\n";
        std::this_thread::sleep_for(std::chrono::minutes(5));
        return 0;
    }
    long long step = 10;
    if (std::ifstream in("test-ecu-step-ms"); in)
    {
        in >> step;
    }
    std::vector<char*> args(argv, argv + argc);
    static char no_control[] = "--no-control";
    bool describe = false;
    for (int i = 1; i < argc; ++i)
    {
        describe = describe || std::string(argv[i]) == "--describe";
    }
    if (std::ifstream("test-ecu-no-control") && !describe)
    {
        args.push_back(no_control);
    }
#ifdef _WIN32
    SetConsoleCtrlHandler(on_stop, TRUE);
#else
    std::signal(SIGTERM, on_stop);
#endif
    return vehicle_sil::ecu_main<TestEcu>(static_cast<int>(args.size()), args.data(),
                                          {.name = "TestEcu", .step = std::chrono::milliseconds(step)});
}
