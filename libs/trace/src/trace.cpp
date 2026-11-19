// SPDX-License-Identifier: MIT
#include "vehicle_sil/trace/trace.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace vehicle_sil::trace {

namespace detail {

Enabled g_enabled;

} // namespace detail

namespace {

/// One record. Its fields are written and read through std::atomic_ref (relaxed: a plain store and
/// load on x86-64 and ARM64), because flush() may read a ring while its thread still writes it.
/// Trivially default constructible: a new ring's pages stay untouched until they are used.
struct Record
{
    std::int64_t start;
    std::int64_t duration;
    std::int64_t vt;
    std::uint64_t arg;
    const char* name;
    std::uint64_t kind;
};

template <typename T>
void put(T& field, T value) noexcept
{
    std::atomic_ref<T>(field).store(value, std::memory_order_relaxed);
}

template <typename T>
T get(T& field) noexcept
{
    return std::atomic_ref<T>(field).load(std::memory_order_relaxed);
}

/// The ring of one thread. Never freed: a thread may still record while the process ends.
struct Buffer
{
    std::uint64_t tid = 0;
    std::unique_ptr<Record[]> ring;
    std::uint64_t capacity = 0; ///< a power of two
    std::atomic<std::uint64_t> count{0}; ///< records ever written; the last `capacity` are kept
    std::atomic<const char*> name{nullptr};
};

struct Tracer
{
    std::mutex mutex;
    std::vector<std::unique_ptr<Buffer>> buffers; ///< under mutex
    std::string process_name;                     ///< under mutex
    bool flushed = false;                         ///< under mutex
    std::filesystem::path directory;
    std::uint64_t capacity = 0;
    std::int64_t start_ns = 0;
};

constexpr std::uint64_t kDefaultCapacity = std::uint64_t{1} << 18; // 12 MB of records per thread
constexpr std::uint64_t kMaxCapacity = std::uint64_t{1} << 24;     // 768 MB of records per thread

std::mutex g_start_mutex;
std::atomic<bool> g_allocation_failed{false};
/// Set once by start() and never freed, so that a record during the end of the process finds it.
std::atomic<Tracer*> g_tracer{nullptr};
thread_local Buffer* t_buffer = nullptr;

std::uint64_t current_tid() noexcept
{
#ifdef _WIN32
    return GetCurrentThreadId();
#else
    return static_cast<std::uint64_t>(::syscall(SYS_gettid));
#endif
}

unsigned long current_pid() noexcept
{
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<unsigned long>(::getpid());
#endif
}

Buffer* thread_buffer() noexcept
{
    if (t_buffer != nullptr)
    {
        return t_buffer;
    }
    Tracer* tracer = g_tracer.load(std::memory_order_acquire);
    if (tracer == nullptr)
    {
        return nullptr;
    }
    try
    {
        auto buffer = std::make_unique<Buffer>();
        buffer->tid = current_tid();
        buffer->capacity = tracer->capacity;
        buffer->ring.reset(new Record[tracer->capacity]); // default-initialised: not touched
        const std::lock_guard lock(tracer->mutex);
        if (tracer->flushed)
        {
            return nullptr;
        }
        t_buffer = buffer.get();
        tracer->buffers.push_back(std::move(buffer));
    }
    catch (const std::bad_alloc&)
    {
        // This thread records nothing. Said once per process (the thread may record again).
        if (!g_allocation_failed.exchange(true))
        {
            std::fprintf(stderr,
                         "vehicle-sil trace: no memory for a ring of %llu records; a thread of this process records nothing. "
                         "Lower VEHICLE_SIL_TRACE_EVENTS.\n",
                         static_cast<unsigned long long>(tracer->capacity));
        }
        return nullptr;
    }
    return t_buffer;
}

/// A name fit for a file name: letters, digits, '-' and '_'.
std::string file_name_part(std::string_view name)
{
    std::string out;
    for (const char c : name)
    {
        const bool fit = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        out += fit ? c : '_';
    }
    return out.empty() ? std::string("process") : out;
}

void write_string(std::FILE* f, const char* text)
{
    std::fputc('"', f);
    for (const char* p = text; *p != '\0'; ++p)
    {
        const auto c = static_cast<unsigned char>(*p);
        if (c == '"' || c == '\\')
        {
            std::fputc('\\', f);
            std::fputc(c, f);
        }
        else if (c < 0x20)
        {
            std::fprintf(f, "\\u%04x", c);
        }
        else
        {
            std::fputc(c, f);
        }
    }
    std::fputc('"', f);
}

/// ns as µs with three decimals, without floating point.
void write_us(std::FILE* f, std::int64_t ns)
{
    if (ns < 0)
    {
        std::fputc('-', f);
        ns = -ns;
    }
    std::fprintf(f, "%lld.%03lld", static_cast<long long>(ns / 1000), static_cast<long long>(ns % 1000));
}

void write_record(std::FILE* f, unsigned long pid, std::uint64_t tid, Record& r)
{
    // A slot that its thread overwrites meanwhile may mix two records; every field is still one
    // that a record had (a literal name, a kind).
    const char* name = get(r.name);
    const std::uint64_t kind = get(r.kind);
    const std::int64_t start = get(r.start);
    std::fprintf(f, ",\n{\"pid\":%lu,\"tid\":%llu,\"name\":", pid, static_cast<unsigned long long>(tid));
    write_string(f, name);
    std::fputs(",\"ts\":", f);
    write_us(f, start);
    switch (static_cast<detail::Kind>(kind))
    {
    case detail::Kind::Span:
        std::fputs(",\"ph\":\"X\",\"dur\":", f);
        write_us(f, get(r.duration));
        std::fprintf(f, ",\"args\":{\"vt\":%lld,\"arg\":%llu}}", static_cast<long long>(get(r.vt)),
                     static_cast<unsigned long long>(get(r.arg)));
        break;
    case detail::Kind::Instant:
        std::fprintf(f, ",\"ph\":\"i\",\"s\":\"t\",\"args\":{\"vt\":%lld,\"arg\":%llu}}", static_cast<long long>(get(r.vt)),
                     static_cast<unsigned long long>(get(r.arg)));
        break;
    case detail::Kind::FlowOut:
        std::fprintf(f, ",\"ph\":\"s\",\"cat\":\"flow\",\"id\":%llu}", static_cast<unsigned long long>(get(r.arg)));
        break;
    case detail::Kind::FlowIn:
        std::fprintf(f, ",\"ph\":\"f\",\"bp\":\"e\",\"cat\":\"flow\",\"id\":%llu}", static_cast<unsigned long long>(get(r.arg)));
        break;
    }
}

std::FILE* open_for_writing(const std::filesystem::path& path)
{
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

/// Writes the file; the tracer's mutex is held.
std::optional<std::filesystem::path> write_file(Tracer& tracer, Errors errors)
{
    const unsigned long pid = current_pid();
    const std::string process = file_name_part(tracer.process_name);
    const std::filesystem::path path = tracer.directory / ("trace-" + process + "-" + std::to_string(pid) + ".json");
    std::error_code ec;
    std::filesystem::create_directories(tracer.directory, ec);
    std::FILE* f = open_for_writing(path);
    if (f == nullptr)
    {
        const int error = errno;
        if (errors == Errors::Silent)
        {
            return std::nullopt;
        }
        std::fprintf(stderr, "vehicle-sil trace: cannot write %s: %s. Set VEHICLE_SIL_TRACE to a directory this process can write to.\n",
                     path.string().c_str(), std::strerror(error));
        return std::nullopt;
    }
    std::vector<char> io(std::size_t{1} << 20);
    std::setvbuf(f, io.data(), _IOFBF, io.size());

    std::uint64_t dropped = 0;
    for (const auto& b : tracer.buffers)
    {
        const std::uint64_t n = b->count.load(std::memory_order_acquire);
        dropped += n > b->capacity ? n - b->capacity : 0;
    }
    const auto system_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::fprintf(f,
                 "{\"displayTimeUnit\":\"ns\",\"otherData\":{\"vehicle_sil_trace\":1,\"pid\":%lu,\"process\":\"%s\","
                 "\"steady_ns\":%lld,\"system_ns\":%lld,\"start_steady_ns\":%lld,\"events_per_thread\":%llu,\"dropped\":%llu},\n"
                 "\"traceEvents\":[\n{\"ph\":\"M\",\"pid\":%lu,\"tid\":0,\"name\":\"process_name\",\"args\":{\"name\":\"%s\"}}",
                 pid, process.c_str(), static_cast<long long>(detail::now_ns()), static_cast<long long>(system_ns),
                 static_cast<long long>(tracer.start_ns), static_cast<unsigned long long>(tracer.capacity),
                 static_cast<unsigned long long>(dropped), pid, process.c_str());
    for (const auto& b : tracer.buffers)
    {
        std::fprintf(f, ",\n{\"ph\":\"M\",\"pid\":%lu,\"tid\":%llu,\"name\":\"thread_name\",\"args\":{\"name\":", pid,
                     static_cast<unsigned long long>(b->tid));
        if (const char* name = b->name.load(std::memory_order_acquire); name != nullptr)
        {
            write_string(f, name);
        }
        else
        {
            std::fprintf(f, "\"thread-%llu\"", static_cast<unsigned long long>(b->tid));
        }
        std::fputs("}}", f);
        const std::uint64_t n = b->count.load(std::memory_order_acquire);
        for (std::uint64_t i = n > b->capacity ? n - b->capacity : 0; i < n; ++i)
        {
            write_record(f, pid, b->tid, b->ring[i & (b->capacity - 1)]);
        }
    }
    std::fputs("\n]}\n", f);
    const bool failed = std::ferror(f) != 0;
    if (std::fclose(f) != 0 || failed)
    {
        if (errors == Errors::Silent)
        {
            return std::nullopt;
        }
        std::fprintf(stderr, "vehicle-sil trace: writing %s failed (is the disk full?); the file is incomplete.\n",
                     path.string().c_str());
        return std::nullopt;
    }
    return path;
}

/// VEHICLE_SIL_TRACE and VEHICLE_SIL_TRACE_EVENTS, read once as the process starts.
bool start_from_environment()
{
#ifdef _WIN32
    const wchar_t* directory = _wgetenv(L"VEHICLE_SIL_TRACE");
    if (directory == nullptr || *directory == L'\0')
    {
        return false;
    }
#else
    const char* directory = std::getenv("VEHICLE_SIL_TRACE");
    if (directory == nullptr || *directory == '\0')
    {
        return false;
    }
#endif
    std::uint64_t events = kDefaultCapacity;
    if (const char* text = std::getenv("VEHICLE_SIL_TRACE_EVENTS"); text != nullptr && *text != '\0')
    {
        char* end = nullptr;
        errno = 0;
        // strtoull() takes "-1" as the largest number: only digits count.
        const bool digits = *text >= '0' && *text <= '9';
        const unsigned long long value = digits ? std::strtoull(text, &end, 10) : 0;
        if (!digits || errno != 0 || *end != '\0' || value == 0 || value > kMaxCapacity)
        {
            std::fprintf(stderr,
                         "vehicle-sil trace: VEHICLE_SIL_TRACE_EVENTS=%s is not a number of records per thread from 1 to %llu; "
                         "using %llu. Set it to a whole number such as 1048576, or unset it.\n",
                         text, static_cast<unsigned long long>(kMaxCapacity), static_cast<unsigned long long>(events));
        }
        else
        {
            events = value;
        }
    }
    // A relative directory is the one of this process's current directory, also for the processes it
    // starts (the launcher starts the ECUs in the configuration file's directory): they inherit the
    // absolute path.
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(std::filesystem::path(directory), ec);
    if (ec)
    {
        absolute = std::filesystem::path(directory);
    }
    else
    {
#ifdef _WIN32
        _wputenv_s(L"VEHICLE_SIL_TRACE", absolute.c_str());
        SetEnvironmentVariableW(L"VEHICLE_SIL_TRACE", absolute.c_str());
#else
        ::setenv("VEHICLE_SIL_TRACE", absolute.c_str(), 1);
#endif
    }
    return start(absolute, events);
}

void flush_at_exit()
{
    static_cast<void>(flush());
}

[[maybe_unused]] const bool g_started_from_environment = start_from_environment();

} // namespace

namespace detail {

std::int64_t now_ns() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void record(Kind kind, const char* name, std::int64_t start_ns, std::int64_t duration_ns, std::int64_t vt, std::uint64_t arg) noexcept
{
    Buffer* b = thread_buffer();
    if (b == nullptr)
    {
        return;
    }
    const std::uint64_t i = b->count.load(std::memory_order_relaxed);
    Record& r = b->ring[i & (b->capacity - 1)];
    put(r.start, start_ns);
    put(r.duration, duration_ns);
    put(r.vt, vt);
    put(r.arg, arg);
    put(r.name, name);
    put(r.kind, static_cast<std::uint64_t>(kind));
    b->count.store(i + 1, std::memory_order_release);
}

void name_thread(const char* name) noexcept
{
    if (Buffer* b = thread_buffer(); b != nullptr)
    {
        b->name.store(name, std::memory_order_release);
    }
}

} // namespace detail

bool start(const std::filesystem::path& directory, std::uint64_t events_per_thread)
{
    const std::lock_guard lock(g_start_mutex);
    if (g_tracer.load(std::memory_order_acquire) != nullptr)
    {
        return false;
    }
    auto* tracer = new Tracer; // never freed (see g_tracer)
    tracer->directory = directory;
    tracer->capacity = 2;
    while (tracer->capacity < events_per_thread && tracer->capacity < kMaxCapacity)
    {
        tracer->capacity <<= 1;
    }
    tracer->start_ns = detail::now_ns();
    g_tracer.store(tracer, std::memory_order_release);
    std::atexit(flush_at_exit);
    detail::g_enabled.on.store(true, std::memory_order_release);
    return true;
}

std::optional<std::filesystem::path> flush(Errors errors)
{
    Tracer* tracer = g_tracer.load(std::memory_order_acquire);
    if (tracer == nullptr)
    {
        return std::nullopt;
    }
    const std::lock_guard lock(tracer->mutex);
    if (tracer->flushed)
    {
        return std::nullopt;
    }
    tracer->flushed = true;
    detail::g_enabled.on.store(false, std::memory_order_relaxed);
    if (tracer->buffers.empty())
    {
        return std::nullopt; // no thread recorded (an ECU asked only for --describe, say): no file
    }
    try
    {
        return write_file(*tracer, errors);
    }
    catch (const std::exception& e)
    {
        if (errors == Errors::Silent)
        {
            return std::nullopt;
        }
        std::fprintf(stderr, "vehicle-sil trace: the file was not written: %s\n", e.what());
        return std::nullopt;
    }
}

void set_process_name(std::string_view name)
{
    Tracer* tracer = g_tracer.load(std::memory_order_acquire);
    if (tracer == nullptr || name.empty())
    {
        return;
    }
    const std::lock_guard lock(tracer->mutex);
    if (tracer->process_name.empty())
    {
        tracer->process_name = name;
    }
}

} // namespace vehicle_sil::trace
