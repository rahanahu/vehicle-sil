// SPDX-License-Identifier: MIT
//
// The device mark on each OS (device_mark.hpp): an abstract Unix socket on Linux, a named mutex
// on Windows, nothing elsewhere.
#include "vehicle_sil/bridge_runtime/device_mark.hpp"

#include <chrono>
#include <random>
#include <utility>

#if defined(__linux__)
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <sddl.h>
#endif

namespace vehicle_sil::bridge_runtime {

#if defined(__linux__)

namespace {

/// Tries with another random part where the name is taken (a container with its own PID namespace
/// in the same network namespace can have the same process id).
constexpr int kBindAttempts = 4;

std::uint64_t random_part()
{
    std::random_device device;
    const std::uint64_t high = device();
    const std::uint64_t low = device();
    return ((high << 32) | low) ^ static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
}

std::string read_proc_net_unix()
{
    std::ifstream file("/proc/net/unix", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

struct DeviceMark::Impl
{
    std::string device;
    bool marked = false;
    std::string why_not;
    std::string prefix; ///< the names of the marks on the device begin with it
    std::string own;    ///< this mark's name, or empty
    int fd = -1;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl()
    {
        if (fd >= 0)
        {
            ::close(fd);
        }
    }
};

DeviceMark DeviceMark::place(std::string_view kind, std::string_view identity)
{
    auto impl = std::make_unique<Impl>();
    impl->device = detail::device_hash(kind, identity);
    impl->prefix = detail::linux_mark_prefix(kind, identity);
    // CLOEXEC: a child process (the bridge starts none today) must not keep the mark alive.
    impl->fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (impl->fd < 0)
    {
        impl->why_not = std::string("no Unix socket for the mark (") + std::strerror(errno) + ")";
        return DeviceMark(std::move(impl));
    }
    int error = 0;
    for (int attempt = 0; attempt < kBindAttempts; ++attempt)
    {
        const std::string name = detail::linux_mark_name(impl->prefix, static_cast<std::uint32_t>(::getpid()), random_part());
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        // The abstract namespace: a NUL first, then the name, without a NUL at the end.
        std::memcpy(address.sun_path + 1, name.data(), name.size());
        const auto length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + name.size());
        if (::bind(impl->fd, reinterpret_cast<const sockaddr*>(&address), length) == 0)
        {
            impl->own = name;
            impl->marked = true;
            return DeviceMark(std::move(impl));
        }
        error = errno;
        if (error != EADDRINUSE)
        {
            break;
        }
    }
    impl->why_not = std::string("the Unix socket of the mark could not be bound (") + std::strerror(error) + ")";
    ::close(impl->fd);
    impl->fd = -1;
    return DeviceMark(std::move(impl));
}

DevicePeers DeviceMark::others() const
{
    return detail::read_proc_net_unix(read_proc_net_unix(), impl_->prefix, impl_->own);
}

DevicePeers look_for_marks(std::string_view kind, std::string_view identity)
{
    return detail::read_proc_net_unix(read_proc_net_unix(), detail::linux_mark_prefix(kind, identity), {});
}

#elif defined(_WIN32)

namespace {

constexpr const char* kSpaces[] = {"Global\\", "Local\\"};

/// The names are ASCII.
std::wstring wide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

/// Everyone may open the mark to see that it is there (SYNCHRONIZE), its owner may do anything.
class Everyone
{
public:
    Everyone()
    {
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;0x001F0001;;;OW)(A;;0x00100000;;;WD)", SDDL_REVISION_1,
                                                                 &descriptor_, nullptr) != 0)
        {
            attributes_.nLength = sizeof(attributes_);
            attributes_.lpSecurityDescriptor = descriptor_;
            attributes_.bInheritHandle = FALSE; // a child process must not keep the mark alive
        }
    }
    Everyone(const Everyone&) = delete;
    Everyone& operator=(const Everyone&) = delete;
    ~Everyone()
    {
        if (descriptor_ != nullptr)
        {
            LocalFree(descriptor_);
        }
    }
    /// nullptr where the descriptor could not be made: the defaults (this user only, not inherited).
    SECURITY_ATTRIBUTES* attributes() noexcept { return descriptor_ != nullptr ? &attributes_ : nullptr; }

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

/// Whether a mark with this name is there; the handle is closed at once, so that a mark is gone
/// as soon as its bridge is.
bool seen(const std::wstring& name)
{
    const HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, name.c_str());
    if (handle == nullptr)
    {
        return false;
    }
    CloseHandle(handle);
    return true;
}

DevicePeers count(const std::string& hash, const std::string& own_space, unsigned own_number)
{
    DevicePeers peers;
    for (const char* space : kSpaces)
    {
        for (unsigned number = 0; number < detail::kWindowsMarks; ++number)
        {
            if (space == own_space && number == own_number)
            {
                continue;
            }
            if (seen(wide(detail::windows_mark_name(space, hash, number))))
            {
                ++peers.count;
            }
        }
    }
    return peers;
}

} // namespace

struct DeviceMark::Impl
{
    std::string device;
    bool marked = false;
    std::string why_not;
    HANDLE handle = nullptr; ///< this mark, held until the end
    std::string space;       ///< "Global\" or "Local\" of this mark
    unsigned number = 0;

    Impl() = default;
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    ~Impl()
    {
        if (handle != nullptr)
        {
            CloseHandle(handle);
        }
    }
};

DeviceMark DeviceMark::place(std::string_view kind, std::string_view identity)
{
    auto impl = std::make_unique<Impl>();
    impl->device = detail::device_hash(kind, identity);
    Everyone everyone;
    for (const char* space : kSpaces)
    {
        bool tryLocal = false;
        for (unsigned number = 0; number < detail::kWindowsMarks; ++number)
        {
            const std::wstring name = wide(detail::windows_mark_name(space, impl->device, number));
            SetLastError(ERROR_SUCCESS);
            const HANDLE handle = CreateMutexW(everyone.attributes(), FALSE, name.c_str());
            const DWORD error = GetLastError();
            if (handle != nullptr && error != ERROR_ALREADY_EXISTS)
            {
                impl->handle = handle;
                impl->space = space;
                impl->number = number;
                impl->marked = true;
                return DeviceMark(std::move(impl));
            }
            if (handle != nullptr)
            {
                // Another bridge's mark: not kept open, or it would stay after that bridge ended.
                CloseHandle(handle);
                continue;
            }
            if (error == ERROR_ACCESS_DENIED)
            {
                // Either another user's mark (this user may only see it), or no right to create
                // global names (a user who is not an administrator): then the marks go to this session.
                if (seen(name))
                {
                    continue;
                }
                if (GetLastError() == ERROR_FILE_NOT_FOUND && std::string_view(space) == kSpaces[0])
                {
                    tryLocal = true;
                    break;
                }
                continue;
            }
            impl->why_not = "the named mutex of the mark could not be created (Windows error " + std::to_string(error) + ")";
            return DeviceMark(std::move(impl));
        }
        if (!tryLocal)
        {
            break;
        }
    }
    impl->why_not = "the " + std::to_string(detail::kWindowsMarks) + " marks of the device are all taken";
    return DeviceMark(std::move(impl));
}

DevicePeers DeviceMark::others() const
{
    return count(impl_->device, impl_->marked ? impl_->space : std::string(), impl_->number);
}

DevicePeers look_for_marks(std::string_view kind, std::string_view identity)
{
    return count(detail::device_hash(kind, identity), {}, 0);
}

#else

struct DeviceMark::Impl
{
    std::string device;
    bool marked = false;
    std::string why_not;
};

DeviceMark DeviceMark::place(std::string_view kind, std::string_view identity)
{
    auto impl = std::make_unique<Impl>();
    impl->device = detail::device_hash(kind, identity);
    impl->why_not = "this operating system has no device marks";
    return DeviceMark(std::move(impl));
}

DevicePeers DeviceMark::others() const
{
    return {};
}

DevicePeers look_for_marks(std::string_view, std::string_view)
{
    return {};
}

#endif

DeviceMark::DeviceMark(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
DeviceMark::DeviceMark(DeviceMark&&) noexcept = default;
DeviceMark& DeviceMark::operator=(DeviceMark&&) noexcept = default;
DeviceMark::~DeviceMark() = default;

const std::string& DeviceMark::device() const noexcept
{
    return impl_->device;
}

bool DeviceMark::marked() const noexcept
{
    return impl_->marked;
}

const std::string& DeviceMark::why_not() const noexcept
{
    return impl_->why_not;
}

} // namespace vehicle_sil::bridge_runtime
