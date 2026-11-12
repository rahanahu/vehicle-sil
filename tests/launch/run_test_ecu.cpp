// SPDX-License-Identifier: MIT
//
// An ecu_main executable for the tests of vehicle_sil_launch run, without networks. It steps every
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
//   hang-after-end          after the run, does not end (its destructor waits for ever), until:
//   stop-exit <code>          a stop request (POSIX SIGTERM, Windows Ctrl+Break) ends it with <code>
//   ignore-stop               a stop request is ignored
//   grandchild <helper>     in its first step, starts <helper> (tests/process/process_helper.cpp)
//                           as a process that sleeps, writes "<name>-grandchild.pid", and is not
//                           ended with the ECU
#include "spawn_sleeper.hpp"

#include "vehicle_sil/simulation/ecu_main.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

int g_stop_code = -1;

#ifdef _WIN32
BOOL WINAPI on_stop(DWORD event)
{
    if (event != CTRL_BREAK_EVENT && event != CTRL_C_EVENT)
    {
        return FALSE;
    }
    if (g_stop_code >= 0)
    {
        TerminateProcess(GetCurrentProcess(), static_cast<UINT>(g_stop_code));
    }
    return TRUE;
}
#else
extern "C" void on_stop(int)
{
    ::_exit(g_stop_code);
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
        }
        // Not in the constructor: --describe constructs the ECU too, and must start nothing.
        ecu.at(vehicle_sil::SimTime{}, [this](vehicle_sil::SimTime) {
            ran_ = true;
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
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
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
        g_stop_code = stop_code_;
#ifdef _WIN32
        SetConsoleCtrlHandler(on_stop, TRUE);
#else
        std::signal(SIGTERM, stop_code_ >= 0 ? on_stop : SIG_IGN);
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
};

} // namespace

int main(int argc, char** argv)
{
    long long step = 10;
    if (std::ifstream in("test-ecu-step-ms"); in)
    {
        in >> step;
    }
    return vehicle_sil::ecu_main<TestEcu>(argc, argv, {.name = "TestEcu", .step = std::chrono::milliseconds(step)});
}
