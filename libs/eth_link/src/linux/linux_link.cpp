// SPDX-License-Identifier: MIT
#include "linux_link.hpp"

#include "../os_link_errors.hpp"

#include <net/if.h>
#include <net/if_arp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstring>
#include <utility>

namespace vehicle_sil::eth_link::detail {
namespace {

/// How often the receive thread looks at the interface (link state, still there, drops).
constexpr auto kWatchInterval = std::chrono::milliseconds(250);
/// Frames read in a row before the thread looks at the stop request again.
constexpr int kFramesPerWake = 64;

/// A socket to ask the kernel about interfaces with (any socket does).
Fd control_socket() noexcept
{
    for (const int family : {AF_INET, AF_INET6, AF_UNIX})
    {
        const int fd = ::socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd >= 0)
        {
            return Fd(fd);
        }
    }
    return Fd();
}

LinkState state_of(unsigned flags) noexcept
{
    return (flags & IFF_UP) != 0 && (flags & IFF_RUNNING) != 0 ? LinkState::Up : LinkState::Down;
}

} // namespace

void Fd::reset(int fd) noexcept
{
    if (fd_ >= 0)
    {
        ::close(fd_);
    }
    fd_ = fd;
}

std::optional<InterfaceState> interface_state(const std::string& name)
{
    if (name.empty() || name.size() > kMaxInterfaceNameBytes)
    {
        return std::nullopt;
    }
    const Fd control = control_socket();
    if (!control.valid())
    {
        return std::nullopt;
    }
    ifreq request{};
    std::memcpy(request.ifr_name, name.data(), name.size());
    if (::ioctl(control.get(), SIOCGIFFLAGS, &request) != 0)
    {
        return std::nullopt;
    }
    InterfaceState state;
    state.flags = static_cast<unsigned>(static_cast<unsigned short>(request.ifr_flags));
    ifreq hardware{};
    std::memcpy(hardware.ifr_name, name.data(), name.size());
    if (::ioctl(control.get(), SIOCGIFHWADDR, &hardware) == 0)
    {
        state.link_type = hardware.ifr_hwaddr.sa_family;
    }
    return state;
}

std::vector<std::string> interface_names()
{
    std::vector<std::string> names;
    struct if_nameindex* const list = ::if_nameindex();
    if (list == nullptr)
    {
        return names;
    }
    for (const struct if_nameindex* entry = list; entry->if_index != 0 && entry->if_name != nullptr; ++entry)
    {
        names.emplace_back(entry->if_name);
    }
    ::if_freenameindex(list);
    return names;
}

std::string executable_path()
{
    char path[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path));
    return length > 0 ? std::string(path, static_cast<std::size_t>(length)) : std::string();
}

LinuxLink::LinuxLink(std::string name) : name_(std::move(name)) {}

std::uint64_t LinuxLink::take_overruns(int) noexcept
{
    return 0;
}

void LinuxLink::start(EventHandler handler)
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (thread_.joinable())
    {
        return;
    }
    if (name_.empty() || name_.size() > kMaxInterfaceNameBytes)
    {
        throw bad_interface_name(name_);
    }
    Fd fd = open();
    Fd wake(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK));
    if (!wake.valid())
    {
        throw LinkError(LinkErrorKind::Other, "cannot start the link on '" + name_ + "' (eventfd): " + error_text(errno) + ".");
    }
    interface_index_ = ::if_nametoindex(name_.c_str());
    handler_ = std::move(handler);
    buffer_.assign(kReadBufferBytes, 0);
    reported_state_ = LinkState::Unknown;
    checksum_warned_ = false;
    faulted_.store(false);
    {
        const std::lock_guard errors(error_mutex_);
        last_error_.clear();
    }
    {
        const std::unique_lock lock(fd_mutex_);
        fd_ = std::move(fd);
        wake_ = std::move(wake);
    }
    thread_ = std::thread([this] { run(); });
}

void LinuxLink::stop() noexcept
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (!thread_.joinable())
    {
        return;
    }
    const std::uint64_t one = 1;
    // The thread ends once it sees the eventfd readable; a failed write cannot happen with a
    // counter this small.
    (void)!::write(wake_.get(), &one, sizeof(one));
    thread_.join();
    // The drops since the thread's last look, from this thread: the handler is not running.
    if (!faulted_.load())
    {
        if (const auto dropped = take_overruns(fd_.get()); dropped > 0)
        {
            try
            {
                LinkEvent event;
                event.kind = LinkEventKind::Overrun;
                event.count = dropped;
                handler_(event);
            }
            catch (...)
            {
            }
        }
    }
    {
        const std::unique_lock lock(fd_mutex_);
        fd_.reset();
        wake_.reset();
    }
    handler_ = nullptr;
}

TransmitResult LinuxLink::transmit(std::span<const std::uint8_t> frame)
{
    const std::shared_lock lock(fd_mutex_);
    if (!fd_.valid())
    {
        set_last_error("the link on '" + name_ + "' is not started");
        return TransmitResult::Failed;
    }
    if (faulted_.load())
    {
        return TransmitResult::Failed; // last_error() tells the fault
    }
    if (write_frame(fd_.get(), frame) >= 0)
    {
        return TransmitResult::Ok;
    }
    const int error = errno;
    if (error == EAGAIN || error == EWOULDBLOCK || error == ENOBUFS)
    {
        return TransmitResult::QueueFull;
    }
    if (error == EIO || error == ENETDOWN)
    {
        set_last_error("sending on '" + name_ + "' failed: " + error_text(error) + " (the interface is down? 'sudo ip link set " +
                       name_ + " up')");
    }
    else
    {
        set_last_error("sending on '" + name_ + "' failed: " + error_text(error));
    }
    return TransmitResult::Failed;
}

std::string LinuxLink::last_error() const
{
    const std::lock_guard lock(error_mutex_);
    return last_error_;
}

void LinuxLink::set_last_error(std::string text)
{
    const std::lock_guard lock(error_mutex_);
    last_error_ = std::move(text);
}

void LinuxLink::fault(const std::string& detail)
{
    set_last_error(detail);
    faulted_.store(true);
    LinkEvent event;
    event.kind = LinkEventKind::Fault;
    event.detail = detail;
    handler_(event);
}

bool LinuxLink::watch(int fd)
{
    const auto state = interface_state(name_);
    if (!state || ::if_nametoindex(name_.c_str()) != interface_index_)
    {
        fault("interface '" + name_ + "' is gone (removed or renamed while the bridge ran)");
        return false;
    }
    if (const auto now = state_of(state->flags); now != reported_state_)
    {
        reported_state_ = now;
        LinkEvent event;
        event.kind = LinkEventKind::LinkState;
        event.state = now;
        handler_(event);
    }
    if (const auto dropped = take_overruns(fd); dropped > 0)
    {
        LinkEvent event;
        event.kind = LinkEventKind::Overrun;
        event.count = dropped;
        handler_(event);
    }
    return true;
}

void LinuxLink::run()
{
    // fd_ and wake_ do not change while this thread runs (stop() joins it before it closes them).
    const int fd = fd_.get();
    const int wake = wake_.get();

    if (const auto state = interface_state(name_); state && (state->flags & IFF_UP) == 0)
    {
        const std::string text = interface_down_warning(name_);
        LinkEvent event;
        event.kind = LinkEventKind::Warning;
        event.detail = text;
        handler_(event);
    }
    bool alive = watch(fd);
    auto watched = std::chrono::steady_clock::now();

    for (;;)
    {
        pollfd fds[2] = {{wake, POLLIN, 0}, {fd, static_cast<short>(alive ? POLLIN : 0), 0}};
        const int ready = ::poll(fds, alive ? 2 : 1, alive ? static_cast<int>(kWatchInterval.count()) : -1);
        if (ready < 0 && errno != EINTR)
        {
            if (alive)
            {
                fault("waiting for frames on '" + name_ + "' failed: " + error_text(errno));
                alive = false;
            }
            continue;
        }
        if (ready > 0 && fds[0].revents != 0)
        {
            return;
        }
        if (!alive)
        {
            continue;
        }
        if (ready > 0 && fds[1].revents != 0)
        {
            for (int i = 0; i < kFramesPerWake && alive; ++i)
            {
                const ReadResult result = read_frame(fd, buffer_);
                if (result.status == ReadStatus::Frame)
                {
                    if (result.checksum_incomplete && !checksum_warned_)
                    {
                        // Once: every frame of the OS's own TCP and UDP is like this.
                        checksum_warned_ = true;
                        const std::string text = checksum_incomplete_warning(name_, result.sent_here);
                        LinkEvent warning;
                        warning.kind = LinkEventKind::Warning;
                        warning.detail = text;
                        handler_(warning);
                    }
                    LinkEvent event;
                    event.kind = LinkEventKind::Frame;
                    event.frame = result.frame;
                    event.checksum_incomplete = result.checksum_incomplete;
                    handler_(event);
                }
                else if (result.status == ReadStatus::Empty)
                {
                    break;
                }
                else if (result.status == ReadStatus::Error)
                {
                    // An interface that went down or away shows here first: tell which.
                    alive = watch(fd);
                    watched = std::chrono::steady_clock::now();
                    if (alive && result.error != ENETDOWN && result.error != EINTR && result.error != ENOBUFS &&
                        result.error != ENOMEM)
                    {
                        fault("reading from '" + name_ + "' failed: " + error_text(result.error));
                        alive = false;
                    }
                    break;
                }
            }
        }
        if (alive && std::chrono::steady_clock::now() - watched >= kWatchInterval)
        {
            alive = watch(fd);
            watched = std::chrono::steady_clock::now();
        }
    }
}

} // namespace vehicle_sil::eth_link::detail
