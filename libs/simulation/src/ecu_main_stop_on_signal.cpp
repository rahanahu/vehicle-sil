// SPDX-License-Identifier: MIT
#include "ecu_main_stop_on_signal.hpp"

#include "ecu_main_describe_stdout.hpp"
#include "ecu_main_text.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/ecu/executable.hpp"
#include "vehicle_sil/peer/peer.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace vehicle_sil::ecu_main_detail {

namespace {

// The exit code of an interrupted ECU, as vehicle_sil_launch knows it (vehicle_sil/ecu/executable.hpp).
using ecu_executable::kExitInterrupted;

} // namespace

StopOnSignal::StopOnSignal(std::string name) : name_(std::move(name))
{
    instance_ = this;
#ifdef _WIN32
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0)
    {
        return; // without the pipe, the signals keep their default effect
    }
    ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    ::fcntl(fds[1], F_SETFD, FD_CLOEXEC);
    read_ = fds[0];
    write_ = fds[1];
    try
    {
        thread_ = std::thread([this] { wait_for_signals(); });
    }
    catch (...)
    {
        // Without the thread, the signals keep their default effect.
        ::close(read_);
        ::close(write_);
        read_ = write_ = -1;
        return;
    }
    struct sigaction action{};
    action.sa_handler = on_signal;
    action.sa_flags = SA_RESTART; // the application's read() and the like are not cut short
    sigemptyset(&action.sa_mask);
    // A signal ignored by whoever started the ECU (nohup) stays ignored.
    for (auto [sig, old] : {std::pair{SIGINT, &old_int_}, std::pair{SIGTERM, &old_term_}})
    {
        if (sigaction(sig, nullptr, old) == 0 && old->sa_handler != SIG_IGN)
        {
            sigaction(sig, &action, nullptr);
        }
    }
#endif
}

StopOnSignal::~StopOnSignal()
{
#ifdef _WIN32
    {
        const std::lock_guard lock(mutex_);
        done_ = true;
    }
    done_changed_.notify_all();
    SetConsoleCtrlHandler(on_console_event, FALSE);
#else
    if (write_ >= 0)
    {
        sigaction(SIGINT, &old_int_, nullptr);
        sigaction(SIGTERM, &old_term_, nullptr);
        const char end = 0;
        static_cast<void>(write_fd(write_, &end, 1));
        thread_.join();
        ::close(read_);
        ::close(write_);
    }
#endif
    instance_ = nullptr;
}

#ifdef _WIN32
void StopOnSignal::run_returned()
{
    {
        const std::lock_guard lock(mutex_);
        done_ = true;
    }
    done_changed_.notify_all();
}
#endif

std::string StopOnSignal::stopped_by() const
{
    const int signal = stopped_by_.load();
    return signal == 0 ? std::string{} : signal_text(signal);
}

std::string StopOnSignal::signal_text(int signal)
{
#ifdef _WIN32
    return signal == CTRL_C_EVENT + 1 ? "Ctrl+C" : signal == CTRL_BREAK_EVENT + 1 ? "Ctrl+Break" : "console closed";
#else
    return signal == SIGINT ? "SIGINT" : signal == SIGTERM ? "SIGTERM" : "signal " + std::to_string(signal);
#endif
}

void StopOnSignal::flush_briefly()
{
    struct FlushState
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    auto state = std::make_shared<FlushState>();
    try
    {
        std::thread([state] {
            static_cast<void>(std::fflush(nullptr));
            {
                const std::lock_guard lock(state->mutex);
                state->done = true;
            }
            state->cv.notify_all();
        }).detach();
    }
    catch (...)
    {
        return; // no thread: the process ends without the flush
    }
    std::unique_lock lock(state->mutex);
    state->cv.wait_for(lock, std::chrono::milliseconds(500), [&state] { return state->done; });
}

void StopOnSignal::handle(int signal)
{
    Peer* peer = peer_.load();
    const auto state = peer != nullptr ? peer->runtime().state() : SilKit::Services::Orchestration::ParticipantState::Invalid;
    using State = SilKit::Services::Orchestration::ParticipantState;
    const bool running = state != State::Invalid && state != State::ServicesCreated &&
                         state != State::CommunicationInitializing && state != State::CommunicationInitialized &&
                         state != State::ReadyToRun;
    int none = 0;
    if (running && stopped_by_.compare_exchange_strong(none, signal))
    {
        peer->request_stop("stopped by " + signal_text(signal));
        return;
    }
    const std::string message = "error: " + one_line(name_) + ": interrupted (" + signal_text(signal) + ") " +
                                (running ? "again while it stopped: it ends at once\n"
                                         : "before the simulation started: it ends at once\n");
    flush_briefly();
    static_cast<void>(write_fd(2, message.data(), message.size()));
#ifdef _WIN32
    TerminateProcess(GetCurrentProcess(), kExitInterrupted);
#else
    ::_exit(kExitInterrupted);
#endif
}

#ifdef _WIN32
BOOL WINAPI StopOnSignal::on_console_event(DWORD event)
{
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT)
    {
        return FALSE;
    }
    StopOnSignal* self = instance_.load();
    if (self == nullptr)
    {
        return FALSE;
    }
    self->handle(static_cast<int>(event) + 1);
    if (event == CTRL_CLOSE_EVENT)
    {
        // Windows ends the process when this returns: give the run time to stop in order (as
        // vehicle_sil_launch does).
        std::unique_lock lock(self->mutex_);
        self->done_changed_.wait_for(lock, std::chrono::seconds(4), [self] { return self->done_; });
    }
    return TRUE;
}
#else
void StopOnSignal::on_signal(int signal)
{
    const int saved = errno; // the interrupted code may look at errno
    StopOnSignal* self = instance_;
    if (self != nullptr && self->write_ >= 0)
    {
        const char byte = static_cast<char>(signal);
        static_cast<void>(write_fd(self->write_, &byte, 1));
    }
    errno = saved;
}

void StopOnSignal::wait_for_signals()
{
    while (true)
    {
        char byte = 0;
        const ssize_t got = ::read(read_, &byte, 1);
        if (got < 0 && errno == EINTR)
        {
            continue;
        }
        if (got <= 0 || byte == 0)
        {
            return;
        }
        handle(byte);
    }
}
#endif

} // namespace vehicle_sil::ecu_main_detail
