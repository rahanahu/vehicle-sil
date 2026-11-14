// SPDX-License-Identifier: MIT
//
// POSIX: loads libpcap with dlopen() and waits for packets with poll() on the descriptor the
// library gives. Only loading, waiting and the path of this program are here; the link's logic is
// in pcap_link.cpp.
#include "pcap_api.hpp"

#include <dlfcn.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __linux__
#include "../linux/linux_link.hpp"

#include <net/if.h>
#endif

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstring>

namespace vehicle_sil::eth_link::detail {
namespace {

// The library's packet header begins with the struct timeval of this OS's headers (sys/time.h).
// A system where it is not two longs (a 64 bit time on a 32 bit system) needs its own header.
static_assert(sizeof(timeval) == 2 * sizeof(long) && offsetof(timeval, tv_usec) == offsetof(PcapPacketHeader, microseconds) &&
                  offsetof(PcapPacketHeader, captured_bytes) == sizeof(timeval) && sizeof(PcapPacketHeader) == sizeof(timeval) + 8,
              "PcapPacketHeader (pcap_api.hpp) does not begin like this system's struct timeval: write it for this system");

/// The names libpcap is installed under: the first is the library's own (Fedora and others), the
/// second is Debian's and Ubuntu's.
constexpr const char* kLibraryNames[] = {"libpcap.so.1", "libpcap.so.0.8"};
/// How long a wait lasts when the library has no descriptor to wait on.
constexpr int kWaitWithoutDescriptorMs = 10;

int milliseconds_of(std::chrono::milliseconds time) noexcept
{
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(time.count(), 0, INT_MAX));
}

class PosixWaiter final : public PcapWaiter
{
public:
    /// `descriptor`: readable when a packet is there, or -1.
    explicit PosixWaiter(int descriptor) : descriptor_(descriptor)
    {
        if (::pipe(stop_) != 0)
        {
            throw LinkError(LinkErrorKind::Other, std::string("cannot start the pcap link (pipe): ") + std::strerror(errno) + ".");
        }
        for (const int end : stop_)
        {
            ::fcntl(end, F_SETFD, FD_CLOEXEC);
            ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
        }
    }
    ~PosixWaiter() override
    {
        ::close(stop_[0]);
        ::close(stop_[1]);
    }

    PcapWake wait(std::chrono::milliseconds limit) override
    {
        pollfd fds[2] = {{stop_[0], POLLIN, 0}, {descriptor_, POLLIN, 0}};
        const bool both = descriptor_ >= 0;
        const int ready = ::poll(fds, both ? 2 : 1, both ? milliseconds_of(limit) : std::min(milliseconds_of(limit), kWaitWithoutDescriptorMs));
        if (ready > 0 && fds[0].revents != 0)
        {
            return PcapWake::Stop;
        }
        if (!both)
        {
            return PcapWake::Ready; // the only way to know is to read
        }
        // An error of the descriptor is "ready" too: reading tells what it is.
        return ready > 0 || (ready < 0 && errno != EINTR) ? PcapWake::Ready : PcapWake::TimedOut;
    }
    bool pause(std::chrono::milliseconds time) override
    {
        pollfd stop = {stop_[0], POLLIN, 0};
        return ::poll(&stop, 1, milliseconds_of(time)) > 0;
    }
    void request_stop() noexcept override
    {
        const char one = 1;
        // The pipe stays readable from now on; a full pipe cannot happen with one byte.
        (void)!::write(stop_[1], &one, 1);
    }

private:
    const int descriptor_;
    int stop_[2] = {-1, -1};
};

#ifdef __linux__
/// libpcap's device names are the kernel's interface names on Linux.
class LinuxInterfaceWatch final : public PcapInterfaceWatch
{
public:
    LinuxInterfaceWatch(std::string name, unsigned index) : name_(std::move(name)), index_(index) {}

    PcapInterfaceState look() override
    {
        const auto state = interface_state(name_);
        if (!state || ::if_nametoindex(name_.c_str()) != index_)
        {
            return PcapInterfaceState::Gone;
        }
        return (state->flags & IFF_UP) != 0 && (state->flags & IFF_RUNNING) != 0 ? PcapInterfaceState::Up : PcapInterfaceState::Down;
    }

private:
    const std::string name_;
    const unsigned index_; // another interface that gets the same name later is not this one
};
#endif

class PosixLibrary final : public PcapLibrary
{
public:
    PosixLibrary(void* handle, std::string path) : PcapLibrary(PcapFlavor::Libpcap, std::move(path)), handle_(handle)
    {
        try
        {
            bind(PcapWaitKind::Descriptor);
        }
        catch (...)
        {
            ::dlclose(handle_);
            throw;
        }
    }
    ~PosixLibrary() override { ::dlclose(handle_); }

    [[nodiscard]] std::unique_ptr<PcapWaiter> make_waiter(PcapHandle* handle) const override
    {
        return std::make_unique<PosixWaiter>(api().get_selectable_fd(handle));
    }
    [[nodiscard]] std::unique_ptr<PcapInterfaceWatch> make_watch(const std::string& name) const override
    {
#ifdef __linux__
        // "any" and the like are libpcap's own devices, not interfaces of the kernel.
        if (const unsigned index = ::if_nametoindex(name.c_str()); index != 0)
        {
            return std::make_unique<LinuxInterfaceWatch>(name, index);
        }
#else
        (void)name;
#endif
        return nullptr;
    }
    [[nodiscard]] std::vector<PcapConnectionName> connection_names() const override { return {}; }
    [[nodiscard]] void* symbol(const char* name) const noexcept override { return ::dlsym(handle_, name); }
    [[nodiscard]] std::string executable_path() const override
    {
        char path[PATH_MAX];
        const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path));
        return length > 0 ? std::string(path, static_cast<std::size_t>(length)) : std::string();
    }

private:
    void* const handle_;
};

} // namespace

std::shared_ptr<PcapLibrary> load_pcap_library(const std::string& library)
{
    std::vector<std::string> names;
    if (library.empty())
    {
        names.assign(std::begin(kLibraryNames), std::end(kLibraryNames));
    }
    else
    {
        names.push_back(library);
    }
    std::string tried;
    std::string why;
    for (const auto& name : names)
    {
        if (void* const handle = ::dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL))
        {
            return std::make_shared<PosixLibrary>(handle, name);
        }
        const char* const error = ::dlerror();
        tried += (tried.empty() ? "" : " or ") + name;
        why = error != nullptr ? error : "unknown error";
    }
    throw pcap_library_missing(PcapFlavor::Libpcap, tried, why);
}

} // namespace vehicle_sil::eth_link::detail
