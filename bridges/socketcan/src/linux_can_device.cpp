// SPDX-License-Identifier: MIT
#include "vehicle_sil/socketcan/can_device.hpp"

#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>
#include <net/if.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::socketcan {
namespace {

std::string error_text(int error)
{
    return std::strerror(error);
}

class LinuxCanDevice final : public CanDevice
{
public:
    explicit LinuxCanDevice(std::string name) : name_(std::move(name)) {}
    ~LinuxCanDevice() override { close(); }

    const std::string& name() const noexcept override { return name_; }

    void open() override
    {
        close();
        const unsigned index = ::if_nametoindex(name_.c_str());
        if (index == 0)
        {
            throw std::runtime_error("there is no interface '" + name_ + "' on this computer");
        }
        const int socket = ::socket(PF_CAN, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, CAN_RAW);
        if (socket < 0)
        {
            const int error = errno;
            throw std::runtime_error("cannot open a raw CAN socket: " + error_text(error) +
                                     (error == EAFNOSUPPORT || error == EPROTONOSUPPORT
                                          ? " (this kernel has no SocketCAN: sudo modprobe can_raw)"
                                          : ""));
        }
        fd_ = socket;
        const int one = 1;
        const can_err_mask_t errors = CAN_ERR_MASK;
        if (::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &one, sizeof(one)) != 0 ||
            ::setsockopt(fd_, SOL_CAN_RAW, CAN_RAW_ERR_FILTER, &errors, sizeof(errors)) != 0)
        {
            const int error = errno;
            close();
            throw std::runtime_error("cannot set up the raw CAN socket on '" + name_ + "': " + error_text(error));
        }
        sockaddr_can address{};
        address.can_family = AF_CAN;
        address.can_ifindex = static_cast<int>(index);
        if (::bind(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            const int error = errno;
            close();
            throw std::runtime_error("cannot open '" + name_ + "': " + error_text(error) +
                                     (error == ENETDOWN ? " (the interface is down: sudo ip link set " + name_ + " up)" : ""));
        }
        wake_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        if (wake_ < 0)
        {
            const int error = errno;
            close();
            throw std::runtime_error("cannot start the bridge on '" + name_ + "' (eventfd): " + error_text(error));
        }
        index_ = index;
    }

    WaitResult wait(std::chrono::milliseconds timeout) override
    {
        pollfd fds[2] = {{wake_, POLLIN, 0}, {fd_, POLLIN, 0}};
        const int ready = ::poll(fds, 2, static_cast<int>(timeout.count()));
        if (ready > 0 && (fds[0].revents & POLLIN) != 0)
        {
            std::uint64_t counter = 0;
            (void)!::read(wake_, &counter, sizeof(counter));
            return WaitResult::Woken;
        }
        // Also POLLERR: the socket holds an error that read() reports.
        return ready > 0 && fds[1].revents != 0 ? WaitResult::Readable : WaitResult::Timeout;
    }

    ReadResult read() override
    {
        ReadResult result;
        can_frame frame{};
        iovec vector{&frame, sizeof(frame)};
        msghdr message{};
        message.msg_iov = &vector;
        message.msg_iovlen = 1;
        const ssize_t size = ::recvmsg(fd_, &message, MSG_DONTWAIT);
        if (size < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return result;
            }
            result.status = ReadStatus::Error;
            result.error = errno;
            return result;
        }
        if (static_cast<std::size_t>(size) < sizeof(can_frame))
        {
            return result; // not a classic frame (the socket gives none): skip it
        }
        result.status = ReadStatus::Frame;
        result.frame.can_id = frame.can_id;
        result.frame.length = frame.len > 8 ? 8 : frame.len;
        std::memcpy(result.frame.data.data(), frame.data, sizeof(frame.data));
        result.frame.confirm = (message.msg_flags & MSG_CONFIRM) != 0;
        return result;
    }

    int write(const RawFrame& raw) override
    {
        can_frame frame{};
        frame.can_id = raw.can_id;
        frame.len = raw.length;
        std::memcpy(frame.data, raw.data.data(), sizeof(frame.data));
        for (;;)
        {
            const ssize_t size = ::send(fd_, &frame, sizeof(frame), MSG_DONTWAIT);
            if (size == static_cast<ssize_t>(sizeof(frame)))
            {
                return 0;
            }
            if (size < 0 && errno == EINTR)
            {
                continue;
            }
            return size < 0 ? errno : EIO;
        }
    }

    std::optional<CanInterfaceInfo> interface() override
    {
        auto found = read_can_interface(index_);
        if (found && found->name == name_)
        {
            return found;
        }
        return std::nullopt; // gone, or renamed
    }

    void wake() noexcept override
    {
        const std::uint64_t one = 1;
        (void)!::write(wake_, &one, sizeof(one));
    }

    void close() noexcept override
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
        }
        if (wake_ >= 0)
        {
            ::close(wake_);
            wake_ = -1;
        }
    }

private:
    const std::string name_;
    unsigned index_ = 0;
    int fd_ = -1;
    int wake_ = -1;
};

} // namespace

std::unique_ptr<CanDevice> make_linux_can_device(std::string name)
{
    return std::make_unique<LinuxCanDevice>(std::move(name));
}

} // namespace vehicle_sil::socketcan
