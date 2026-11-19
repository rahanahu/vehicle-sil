// SPDX-License-Identifier: MIT
//
// vehicle_sil::trace - an internal trace of steps and writes for profiling (docs/internals/trace.md).
//
// Off unless the environment variable VEHICLE_SIL_TRACE names a directory when the process starts
// (or a test calls start()). Off, every function below costs one relaxed atomic load and a branch,
// and allocates nothing. On, each thread that records gets a ring of its own at its first record
// (VEHICLE_SIL_TRACE_EVENTS records, rounded up to a power of two; the oldest are overwritten), and
// at exit, or at flush(), the process writes <directory>/trace-<process>-<pid>.json in the Chrome
// Trace Event format. Times are std::chrono::steady_clock, which every process of a host shares.
// Every record carries the virtual time it belongs to (vt, ns since the start; -1 when it has none)
// and one integer argument.
//
// The file format is an internal tool's, not an interface: it may change with any release.
//
// Names are string literals: only the pointer is kept until the file is written.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string_view>

namespace vehicle_sil::trace {

namespace detail {

/// Whether records are taken. Constant-initialised, so it is false before any constructor runs.
/// A cache line of its own: every place that records reads it, and nothing else shares the line.
struct alignas(64) Enabled
{
    std::atomic<bool> on{false};
};
extern Enabled g_enabled;

enum class Kind : std::uint8_t
{
    Span,    ///< complete event ('X')
    Instant, ///< 'i'
    FlowOut, ///< 's': the start of an arrow, bound to the enclosing span
    FlowIn,  ///< 'f': its end, bound to the enclosing span
};

void record(Kind kind, const char* name, std::int64_t start_ns, std::int64_t duration_ns, std::int64_t vt,
            std::uint64_t arg) noexcept;
std::int64_t now_ns() noexcept;
void name_thread(const char* name) noexcept;

} // namespace detail

/// Whether records are taken.
inline bool enabled() noexcept
{
    return detail::g_enabled.on.load(std::memory_order_relaxed);
}

/// Starts taking records, to be written to `directory` (made at flush() if it does not exist).
/// `events_per_thread` is rounded up to a power of two (at least 2). Returns false, and changes
/// nothing, if records were taken already in this process (one start per process: the
/// environment's, or a test's).
bool start(const std::filesystem::path& directory, std::uint64_t events_per_thread = 1U << 18);

/// Whether flush() says on stderr that the file could not be written.
enum class Errors
{
    ToStderr,
    Silent, ///< for a process that ends while nothing may read its stderr any more
};

/// Writes the file of this process and stops taking records; later calls do nothing. Returns the
/// file's path, or nothing when the trace was not started, no thread recorded anything (no file is
/// written), or the file could not be written. Runs at exit by itself; a process that ends without
/// running atexit (std::_Exit) calls it first. A record that a thread writes meanwhile may come out
/// mixed with the older record of its slot.
std::optional<std::filesystem::path> flush(Errors errors = Errors::ToStderr);

/// The process's name in the file name and in the trace (the first one given counts).
void set_process_name(std::string_view name);

/// The calling thread's name in the trace (a string literal). Without it a thread is
/// "thread-<id>".
inline void set_thread_name(const char* name) noexcept
{
    if (enabled())
    {
        detail::name_thread(name);
    }
}

inline void instant(const char* name, std::int64_t vt = -1, std::uint64_t arg = 0) noexcept
{
    if (enabled())
    {
        detail::record(detail::Kind::Instant, name, detail::now_ns(), 0, vt, arg);
    }
}

/// The start of an arrow to the flow_in() with the same name and id (in another thread or process).
inline void flow_out(const char* name, std::uint64_t id) noexcept
{
    if (enabled())
    {
        detail::record(detail::Kind::FlowOut, name, detail::now_ns(), 0, -1, id);
    }
}

inline void flow_in(const char* name, std::uint64_t id) noexcept
{
    if (enabled())
    {
        detail::record(detail::Kind::FlowIn, name, detail::now_ns(), 0, -1, id);
    }
}

/// The ids of the arrows of one write's round trip to one agent, from the write's seq and the
/// agent's session (a write to several agents has one seq): the command (hub to agent) and the
/// answer (agent to hub). Below 2^53, so that a viewer that reads JSON numbers as doubles keeps them.
constexpr std::uint64_t command_flow(std::uint64_t seq, std::uint64_t session) noexcept
{
    return (((seq << 20) ^ (session & 0xFFFFF)) & ((std::uint64_t{1} << 52) - 1)) * 2;
}
constexpr std::uint64_t answer_flow(std::uint64_t seq, std::uint64_t session) noexcept
{
    return command_flow(seq, session) + 1;
}

/// One record of the time from its construction to its destruction.
class Span
{
public:
    explicit Span(const char* name, std::int64_t vt = -1, std::uint64_t arg = 0) noexcept :
      name_(name), vt_(vt), arg_(arg), start_(enabled() ? detail::now_ns() : -1)
    {
    }
    ~Span()
    {
        if (start_ >= 0 && enabled())
        {
            detail::record(detail::Kind::Span, name_, start_, detail::now_ns() - start_, vt_, arg_);
        }
    }
    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    void set_name(const char* name) noexcept { name_ = name; }
    void set_vt(std::int64_t vt) noexcept { vt_ = vt; }
    void set_arg(std::uint64_t arg) noexcept { arg_ = arg; }

private:
    const char* name_;
    std::int64_t vt_;
    std::uint64_t arg_;
    std::int64_t start_;
};

} // namespace vehicle_sil::trace
