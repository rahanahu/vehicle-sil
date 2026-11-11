// SPDX-License-Identifier: MIT
#include "vehicle_sil/simulation/ecu_main.hpp"

#include "vehicle_sil/core/participant_runtime.hpp"
#include "vehicle_sil/peer/peer.hpp"
#include "vehicle_sil/simulation/simulation.hpp"

#include <atomic>
#include <charconv>
#include <condition_variable>
#include <mutex>
#include <csignal>
#include <cstdio>
#include <chrono>
#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <cerrno>
#include <cstddef>
#include <fcntl.h>

#ifdef _WIN32
#include <io.h>
#include <stdlib.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace vehicle_sil {

namespace {

// The exit codes of the other tools (tools/common/include/vehicle_sil/cli/args.hpp).
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitSetupFailed = 3;
constexpr int kExitUsage = 4;
/// Interrupted by a signal (SIGINT, SIGTERM; Windows Ctrl+C, Ctrl+Break): 128 + SIGINT, as shells
/// report a program that Ctrl+C ended.
constexpr int kExitInterrupted = 130;

/// The longest --run-for-ms: about 31 years, as the other tools' durations.
// vehicle_sil_launch gives at most this as --start-timeout-ms (vehicle_sil::launch::kMaxMillis).
constexpr std::uint64_t kMaxMillis = 1'000'000'000'000;

class UsageError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

struct CommandLine
{
    bool describe = false;
    bool local = false;
    std::optional<std::string> registry_uri;
    std::optional<std::string> name;
    std::optional<std::chrono::milliseconds> run_for;
    std::optional<std::chrono::milliseconds> start_timeout;
};

/// The start timeout without --start-timeout-ms (SilKitOptions::start_timeout).
constexpr std::chrono::milliseconds kDefaultStartTimeout{30000};

/// The options that take a value.
constexpr std::string_view kValueOptions[] = {"--registry-uri", "--name", "--run-for-ms", "--start-timeout-ms"};

bool takes_value(std::string_view arg)
{
    for (const auto option : kValueOptions)
    {
        if (arg == option)
        {
            return true;
        }
    }
    return false;
}

/// The bytes of `text` in hexadecimal ("83 47"), for a text that cannot be shown as it is.
std::string hex_bytes(std::string_view text)
{
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char c : text)
    {
        const auto b = static_cast<unsigned char>(c);
        out += out.empty() ? "" : " ";
        out += kHex[b >> 4];
        out += kHex[b & 0xF];
    }
    return out;
}

std::string quoted(std::string_view text) { return "\"" + std::string(text) + "\""; }

/// "10 ms", "500 us" or "250 ns".
std::string duration_text(SimDuration d)
{
    using namespace std::chrono;
    if (d.count() % duration_cast<SimDuration>(milliseconds(1)).count() == 0)
    {
        return std::to_string(duration_cast<milliseconds>(d).count()) + " ms";
    }
    if (d.count() % duration_cast<SimDuration>(microseconds(1)).count() == 0)
    {
        return std::to_string(duration_cast<microseconds>(d).count()) + " us";
    }
    return std::to_string(duration_cast<nanoseconds>(d).count()) + " ns";
}

/// The file name of argv[0] without its directories ("/" and "\"); `fallback` if there is none.
std::string program_name(int argc, const char* const* argv, const std::string& fallback)
{
    if (argc < 1 || argv == nullptr || argv[0] == nullptr || *argv[0] == '\0')
    {
        return fallback;
    }
    const std::string_view path = argv[0];
    const auto slash = path.find_last_of("/\\");
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    return file.empty() ? fallback : std::string(file);
}

std::string usage(const std::string& program, const EcuMainOptions& options)
{
    const std::string pad(program.size(), ' ');
    return "usage: " + program + " --describe [--name <name>]\n"
           "       " + pad + " --registry-uri <uri> [--start-timeout-ms <ms>] [--name <name>]\n"
           "       " + pad + " --local --run-for-ms <ms> [--name <name>]\n"
           "       " + pad + " --help\n"
           "the ECU " + quoted(options.name) + ", step " + duration_text(options.step) + "\n"
           "  --describe              write what the ECU is (networks, J1939 nodes, variables) as JSON to\n"
           "                          stdout and exit, without connecting or running anything\n"
           "  --registry-uri <uri>    join the SIL Kit simulation at <uri> and run until it ends; a controller\n"
           "                          elsewhere (vehicle_sil_sim_controller) starts and stops it\n"
           "  --start-timeout-ms <ms> with --registry-uri: how long to wait for the start (default 30000)\n"
           "  --local                 run this ECU alone on local virtual buses, for --run-for-ms\n"
           "  --run-for-ms <ms>       how long --local runs (virtual time, > 0)\n"
           "  --name <name>           the participant name instead of " + quoted(options.name) + "; only the name\n"
           "                          changes (not the J1939 NAME, the addresses or the variables)\n"
           "  --help                  write this text to stdout and exit\n"
           "exit codes: 0 done, 1 the ECU failed in the run, 3 the set-up failed (the ECU's set-up, the\n"
           "            registry, the start, writing to stdout), 4 a bad command line\n";
}

std::chrono::milliseconds parse_millis(std::string_view option, std::string_view text)
{
    std::uint64_t value = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 || value > kMaxMillis)
    {
        throw UsageError(std::string(option) + " needs a whole number of milliseconds from 1 to " +
                         std::to_string(kMaxMillis) + ", not " + quoted(text) + " (e.g. " + std::string(option) +
                         " 1000)");
    }
    return std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(value));
}

bool is_help(std::string_view arg) { return arg == "--help" || arg == "-h"; }

/// true if `args` ask for the usage: --help or -h where an option goes, not as the value of an
/// option ("--name -h" names the ECU "-h"; a value that starts with "--" is a missing value).
bool asks_for_help(const std::vector<std::string_view>& args)
{
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        if (takes_value(args[i]) && i + 1 < args.size() && args[i + 1].substr(0, 2) != "--")
        {
            ++i;
        }
        else if (is_help(args[i]))
        {
            return true;
        }
    }
    return false;
}

CommandLine parse(const std::vector<std::string_view>& args)
{
    CommandLine c;
    std::vector<std::string_view> seen;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string_view arg = args[i];
        for (const auto& before : seen)
        {
            if (before == arg)
            {
                throw UsageError(quoted(arg) + " is given twice; give it once");
            }
        }
        // The value after `arg`; an option there is a missing value, not the value.
        const auto value = [&](const char* what, const char* example) {
            if (i + 1 >= args.size() || args[i + 1].substr(0, 2) == "--")
            {
                throw UsageError(std::string(arg) + " needs " + what + " after it (e.g. " + std::string(arg) + " " +
                                 example + ")" +
                                 (i + 1 < args.size() ? ", not the option " + quoted(args[i + 1]) : std::string()));
            }
            return std::string(args[++i]);
        };
        if (arg == "--describe")
        {
            c.describe = true;
        }
        else if (arg == "--local")
        {
            c.local = true;
        }
        else if (arg == "--registry-uri")
        {
            c.registry_uri = value("the registry's URI", "silkit://localhost:8500");
            if (c.registry_uri->empty())
            {
                throw UsageError("--registry-uri needs a URI that is not empty (e.g. --registry-uri silkit://localhost:8500)");
            }
        }
        else if (arg == "--name")
        {
            c.name = value("a name", "Engine2");
            if (c.name->empty())
            {
                throw UsageError("--name needs a name that is not empty (e.g. --name Engine2)");
            }
            if (!detail::is_utf8(*c.name))
            {
                throw UsageError("--name needs ASCII or UTF-8 text, not the bytes " + hex_bytes(*c.name) +
                                 " (a name in another encoding, e.g. a Windows code page)");
            }
        }
        else if (arg == "--start-timeout-ms")
        {
            c.start_timeout = parse_millis(arg, value("a duration in milliseconds", "60000"));
        }
        else if (arg == "--run-for-ms")
        {
            c.run_for = parse_millis(arg, value("a duration in milliseconds", "1000"));
        }
        else
        {
            std::string hint = "see the usage below";
            for (const auto known : kValueOptions)
            {
                if (arg.size() > known.size() && arg.substr(0, known.size()) == known && arg[known.size()] == '=')
                {
                    hint = "write the value after a space: " + std::string(known) + " " +
                           std::string(arg.substr(known.size() + 1));
                }
            }
            throw UsageError("unknown argument " + quoted(arg) + " (" + hint + ")");
        }
        seen.push_back(arg);
    }
    const int modes = (c.describe ? 1 : 0) + (c.local ? 1 : 0) + (c.registry_uri ? 1 : 0);
    if (modes == 0)
    {
        throw UsageError(args.empty() ? "no arguments: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>"
                                      : "nothing to do: give --describe, --registry-uri <uri> or --local --run-for-ms <ms>");
    }
    if (modes > 1)
    {
        throw UsageError("--describe, --registry-uri and --local exclude each other: give one of them");
    }
    if (c.local && !c.run_for)
    {
        throw UsageError("--local needs --run-for-ms <ms>, how much virtual time to run (e.g. --local --run-for-ms 1000)");
    }
    if (c.run_for && !c.local)
    {
        throw UsageError(c.describe ? "--run-for-ms is for --local: --describe runs nothing"
                                    : "--run-for-ms is for --local: with --registry-uri, the controller ends the run "
                                      "(vehicle_sil_sim_controller --stop-at-ms)");
    }
    if (c.start_timeout && !c.registry_uri)
    {
        throw UsageError("--start-timeout-ms is for --registry-uri: " +
                         std::string(c.describe ? "--describe" : "--local") + " starts nothing to wait for");
    }
    return c;
}

/// `what` of a SimulationError of the ECU `name` without the "<name>: " it starts with.
std::string without_name(const std::string& what, const std::string& name)
{
    const std::string prefix = name + ": ";
    return what.compare(0, prefix.size(), prefix) == 0 ? what.substr(prefix.size()) : what;
}

/// `text` on one line: every run of line breaks (SIL Kit's messages have them) becomes a space.
std::string one_line(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool brk = false;
    for (const char c : text)
    {
        if (c == '\n' || c == '\r')
        {
            brk = true;
            continue;
        }
        if (brk && !out.empty() && out.back() != ' ')
        {
            out += ' ';
        }
        brk = false;
        out += c;
    }
    return out;
}

/// Writes "error: <name>: <why>" as one line to stderr; returns `code`.
int fail(const std::string& name, int code, std::string_view why)
{
    std::cerr << "error: " << one_line(name) << ": " << one_line(why) << '\n' << std::flush;
    return code;
}

#ifdef _WIN32
void ignore_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, std::uintptr_t) {}

/// While it lives, a call of the C runtime on this thread with a bad argument (e.g. _dup() or
/// _get_osfhandle() of a descriptor that is not open) returns its error instead of calling the
/// invalid parameter handler, which by default ends the process under the UCRT. The function
/// is looked up in the UCRT at run time (the MinGW-w64 headers do not declare it); with another
/// C runtime (msvcrt.dll, which returns the error anyway) nothing changes.
class QuietCrt
{
public:
    QuietCrt()
    {
        if (const HMODULE crt = GetModuleHandleW(L"ucrtbase.dll"))
        {
            // Through void (*)(): a generic function pointer type for -Wcast-function-type.
            set_ = reinterpret_cast<Set>(reinterpret_cast<void (*)()>(
                GetProcAddress(crt, "_set_thread_local_invalid_parameter_handler")));
        }
        if (set_ != nullptr)
        {
            old_ = set_(&ignore_invalid_parameter);
        }
    }
    ~QuietCrt()
    {
        if (set_ != nullptr)
        {
            set_(old_);
        }
    }
    QuietCrt(const QuietCrt&) = delete;
    QuietCrt& operator=(const QuietCrt&) = delete;

private:
    using Set = _invalid_parameter_handler(__cdecl*)(_invalid_parameter_handler);
    Set set_ = nullptr;
    _invalid_parameter_handler old_ = nullptr;
};

bool fd_is_open(int fd)
{
    // -2: a standard descriptor without an OS handle (a program without a console).
    const intptr_t handle = _get_osfhandle(fd);
    return handle != -1 && handle != -2;
}
int dup_fd(int fd) { return _dup(fd); }
int dup2_fd(int from, int to) { return _dup2(from, to); }
void close_fd(int fd) { static_cast<void>(_close(fd)); }
int open_null() { return _open("NUL", _O_WRONLY | _O_BINARY); }
long write_fd(int fd, const char* data, std::size_t size)
{
    return _write(fd, data, static_cast<unsigned int>(size > 0x40000000u ? 0x40000000u : size));
}
#else
/// Nothing to quiet: POSIX calls return their errors. (Not trivial, so that an unused one is
/// not a warning.)
struct QuietCrt
{
    QuietCrt() noexcept {}
    ~QuietCrt() {}
    QuietCrt(const QuietCrt&) = delete;
    QuietCrt& operator=(const QuietCrt&) = delete;
};
bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }
int dup_fd(int fd) { return ::dup(fd); }
int dup2_fd(int from, int to) { return ::dup2(from, to); }
void close_fd(int fd) { static_cast<void>(::close(fd)); }
int open_null() { return ::open("/dev/null", O_WRONLY); }
long write_fd(int fd, const char* data, std::size_t size) { return static_cast<long>(::write(fd, data, size)); }
#endif

/// --describe's stdout. From its construction to the end of the process, fd 1 points at stderr,
/// so what the application writes to stdout (in its constructor, its destructor, a thread of
/// its own, atexit, a library's banner) goes to stderr; it is never pointed back. The JSON goes
/// to a copy of the original stdout descriptor (write()). If stderr is closed, it is first
/// opened on the null device, so that no copy of stdout lands on descriptor 2; if stdout is
/// closed, fd 1 still points at stderr (no file the application opens becomes fd 1) and write()
/// fails. The C and C++ streams are flushed before the switch.
class DescribeStdout
{
public:
    DescribeStdout()
    {
        std::cout.flush();
        std::cerr.flush();
        static_cast<void>(std::fflush(stdout));
        static_cast<void>(std::fflush(stderr));
        const QuietCrt quiet;
        if (!fd_is_open(kStderrFd))
        {
            const int null = open_null();
            if (null != -1 && null != kStderrFd)
            {
                static_cast<void>(dup2_fd(null, kStderrFd));
                close_fd(null);
            }
        }
        if (fd_is_open(kStdoutFd))
        {
            saved_ = dup_fd(kStdoutFd);
        }
        if (fd_is_open(kStderrFd))
        {
            static_cast<void>(dup2_fd(kStderrFd, kStdoutFd));
        }
#ifdef _WIN32
        if (saved_ != -1)
        {
            static_cast<void>(_setmode(saved_, _O_BINARY)); // "\n" stays "\n"
        }
#endif
    }
    ~DescribeStdout()
    {
        if (saved_ != -1)
        {
            const QuietCrt quiet;
            close_fd(saved_);
        }
    }
    DescribeStdout(const DescribeStdout&) = delete;
    DescribeStdout& operator=(const DescribeStdout&) = delete;

    /// Writes `text` to the original stdout. false if it is closed or the write failed.
    bool write(const std::string& text)
    {
        if (saved_ == -1)
        {
            return false;
        }
        const QuietCrt quiet;
        std::size_t done = 0;
        while (done < text.size())
        {
            const long n = write_fd(saved_, text.data() + done, text.size() - done);
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n <= 0)
            {
                return false;
            }
            done += static_cast<std::size_t>(n);
        }
        return true;
    }

private:
    static constexpr int kStdoutFd = 1;
    static constexpr int kStderrFd = 2;
    int saved_ = -1;
};

/// Writes `text` to stdout as it is: on Windows without turning "\n" into "\r\n", so that the
/// JSON has the same bytes everywhere. false if the write failed.
bool write_stdout(const std::string& text)
{
    std::cout << std::flush;
#ifdef _WIN32
    std::fflush(stdout);
    const int old = _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::cout << text << std::flush;
    const bool ok = static_cast<bool>(std::cout);
#ifdef _WIN32
    if (old != -1)
    {
        static_cast<void>(_setmode(_fileno(stdout), old));
    }
#endif
    return ok;
}

/// --registry-uri: while it lives, SIGINT and SIGTERM (Windows: Ctrl+C, Ctrl+Break, closing the
/// console) end the ECU in order once the simulation runs: the participant stops (which stops the
/// coordinated simulation for every participant), run() returns and the destructors run. Before
/// the simulation runs (connecting, waiting for the start) the process ends at once, as it would
/// by default, with a message and kExitInterrupted. A second signal ends it at once.
class StopOnSignal
{
public:
    explicit StopOnSignal(std::string name) : name_(std::move(name))
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
    ~StopOnSignal()
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
    StopOnSignal(const StopOnSignal&) = delete;
    StopOnSignal& operator=(const StopOnSignal&) = delete;

    /// The participant to stop once it runs.
    void attach(Peer& peer) { peer_.store(&peer); }
#ifdef _WIN32
    /// The run has returned (a handler of a closing console waits for it).
    void run_returned()
    {
        {
            const std::lock_guard lock(mutex_);
            done_ = true;
        }
        done_changed_.notify_all();
    }
#else
    void run_returned() {}
#endif
    /// The signal that stopped the ECU in order ("SIGTERM"), or "".
    [[nodiscard]] std::string stopped_by() const
    {
        const int signal = stopped_by_.load();
        return signal == 0 ? std::string{} : signal_text(signal);
    }

private:
    static std::string signal_text(int signal)
    {
#ifdef _WIN32
        return signal == CTRL_C_EVENT + 1 ? "Ctrl+C" : signal == CTRL_BREAK_EVENT + 1 ? "Ctrl+Break" : "console closed";
#else
        return signal == SIGINT ? "SIGINT" : signal == SIGTERM ? "SIGTERM" : "signal " + std::to_string(signal);
#endif
    }

    /// Writes out what the application wrote with printf() and the like (not in a signal handler:
    /// this runs on handle()'s thread), but waits for it at most 500 ms: fflush() waits for the
    /// lock of each stream, which the main thread may hold for ever (a write to a full pipe).
    static void flush_briefly()
    {
        static std::atomic<bool> flushed{false};
        try
        {
            std::thread([] {
                static_cast<void>(std::fflush(nullptr));
                flushed.store(true);
            }).detach();
        }
        catch (...)
        {
            return; // no thread: the process ends without the flush
        }
        for (int i = 0; i < 100 && !flushed.load(); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    /// On its own thread (POSIX: wait_for_signals(); Windows: the console's handler thread).
    void handle(int signal)
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
    static BOOL WINAPI on_console_event(DWORD event)
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
    static void on_signal(int signal)
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

    void wait_for_signals()
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

    int read_ = -1;
    int write_ = -1;
    struct sigaction old_int_{};
    struct sigaction old_term_{};
    std::thread thread_;
#endif

#ifdef _WIN32
    std::mutex mutex_;
    std::condition_variable done_changed_;
    bool done_ = false; ///< guarded by mutex_
#endif
    static inline std::atomic<StopOnSignal*> instance_{nullptr};
    std::string name_;
    std::atomic<Peer*> peer_{nullptr};
    std::atomic<int> stopped_by_{0};
};

} // namespace

int ecu_main(int argc, const char* const* argv, const EcuMainOptions& options, const std::function<void(Ecu&)>& set_up)
{
    const std::string program = program_name(argc, argv, options.name);
    std::vector<std::string_view> args;
    for (int i = 1; i < argc && argv != nullptr; ++i)
    {
        args.emplace_back(argv[i] != nullptr ? argv[i] : "");
    }
    if (asks_for_help(args))
    {
        if (!write_stdout(usage(program, options)))
        {
            return fail(options.name, kExitSetupFailed, "could not write the usage to stdout (is it closed?)");
        }
        return kExitOk;
    }
    CommandLine c;
    try
    {
        c = parse(args);
    }
    catch (const UsageError& e)
    {
        const int code = fail(options.name, kExitUsage, e.what());
        std::cerr << usage(program, options) << std::flush;
        return code;
    }
    const std::string name = c.name.value_or(options.name);

    std::optional<Simulation> sim;
    std::optional<StopOnSignal> stop_on_signal; // --registry-uri; destroyed before the ECU
    std::optional<DescribeStdout> describe_out; // --describe; destroyed before the ECU, never restored
    Ecu* ecu = nullptr;
    try
    {
        if (!detail::is_utf8(options.name))
        {
            throw std::invalid_argument("EcuMainOptions::name is not ASCII or UTF-8 text (the bytes " +
                                        hex_bytes(options.name) + "): give the ECU a UTF-8 name in main()");
        }
        if (c.registry_uri)
        {
            SilKitOptions silkit;
            silkit.registry_uri = *c.registry_uri;
            silkit.controller = false;
            silkit.start_timeout = c.start_timeout.value_or(kDefaultStartTimeout);
            sim.emplace(silkit);
            stop_on_signal.emplace(name);
        }
        else
        {
            sim.emplace();
        }
        try
        {
            ecu = &sim->add_ecu(name, options.step);
        }
        catch (const std::logic_error&)
        {
            throw; // a bad name or step (std::invalid_argument is a logic_error)
        }
        catch (const std::exception& e)
        {
            if (!c.registry_uri)
            {
                throw;
            }
            // SIL Kit: the participant connects to the registry here.
            throw std::runtime_error("cannot join the simulation at " + quoted(*c.registry_uri) + ": " + e.what() +
                                     " (check the URI, e.g. silkit://localhost:8500, that sil-kit-registry runs there, "
                                     "and that no other participant there is named " + quoted(name) + ")");
        }
        if (stop_on_signal)
        {
            stop_on_signal->attach(sim->peer(name));
        }
        if (!set_up)
        {
            throw std::invalid_argument("no function to set the ECU up (ecu_main()'s set_up is empty)");
        }
        // --describe: from here on, stdout has only the JSON (DescribeStdout).
        if (c.describe)
        {
            describe_out.emplace();
        }
        set_up(*ecu);
    }
    catch (const std::exception& e)
    {
        return fail(name, kExitSetupFailed, e.what());
    }
    catch (...)
    {
        return fail(name, kExitSetupFailed, "the set-up threw an exception that is not a std::exception");
    }

    if (c.describe)
    {
        if (!describe_out->write(to_json(ecu->describe())))
        {
            return fail(name, kExitSetupFailed, "could not write the description to stdout (is it closed?)");
        }
        return kExitOk;
    }
    try
    {
        if (c.local)
        {
            sim->run_for(*c.run_for);
        }
        else
        {
            struct Returned
            {
                StopOnSignal* stop;
                ~Returned()
                {
                    if (stop != nullptr)
                    {
                        stop->run_returned();
                    }
                }
            } const returned{stop_on_signal ? &*stop_on_signal : nullptr};
            sim->run();
        }
    }
    catch (const SimulationError& e)
    {
        if (stop_on_signal && !stop_on_signal->stopped_by().empty())
        {
            return fail(name, kExitInterrupted,
                        "interrupted (" + stop_on_signal->stopped_by() + "): it stopped the simulation and left it (" +
                            without_name(e.what(), name) + ")");
        }
        return fail(name, kExitFailed, without_name(e.what(), name));
    }
    catch (const StartTimeoutError& e)
    {
        return fail(name, kExitSetupFailed,
                    "the simulation did not start: " + std::string(e.what()) +
                        " (is a controller such as vehicle_sil_sim_controller running, with " + quoted(name) +
                        " among its required participants? To wait longer: --start-timeout-ms)");
    }
    catch (const std::exception& e)
    {
        if (c.local)
        {
            return fail(name, kExitFailed, e.what());
        }
        return fail(name, kExitSetupFailed, "could not start: " + std::string(e.what()));
    }
    catch (...)
    {
        return fail(name, kExitFailed, "the run threw an exception that is not a std::exception");
    }
    if (stop_on_signal && !stop_on_signal->stopped_by().empty())
    {
        return fail(name, kExitInterrupted,
                    "interrupted (" + stop_on_signal->stopped_by() + "): it stopped the simulation and left it");
    }
    return kExitOk;
}

} // namespace vehicle_sil
