// SPDX-License-Identifier: MIT
//
// Asks the kernel for the CAN interfaces (RTM_GETLINK, NLM_F_DUMP). Reading needs no privileges.
#include "vehicle_sil/socketcan/can_interfaces.hpp"

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace vehicle_sil::socketcan {
namespace {

class Socket
{
public:
    Socket() : fd_(::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE)) {}
    ~Socket()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    [[nodiscard]] int get() const noexcept { return fd_; }

private:
    int fd_;
};

} // namespace

std::vector<CanInterfaceInfo> read_can_interfaces()
{
    const Socket netlink;
    if (netlink.get() < 0)
    {
        throw std::runtime_error(std::string("cannot ask the kernel for the CAN interfaces (netlink socket): ") +
                                 std::strerror(errno));
    }
    timeval timeout{5, 0};
    ::setsockopt(netlink.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct
    {
        nlmsghdr header;
        ifinfomsg info;
    } request{};
    request.header.nlmsg_len = sizeof(request);
    request.header.nlmsg_type = RTM_GETLINK;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq = 1;
    request.info.ifi_family = AF_UNSPEC;
    if (::send(netlink.get(), &request, sizeof(request), 0) < 0)
    {
        throw std::runtime_error(std::string("cannot ask the kernel for the CAN interfaces (RTM_GETLINK): ") +
                                 std::strerror(errno));
    }

    std::vector<CanInterfaceInfo> result;
    std::array<std::uint8_t, 32768> buffer{};
    for (;;)
    {
        const ssize_t received = ::recv(netlink.get(), buffer.data(), buffer.size(), 0);
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            throw std::runtime_error(std::string("the kernel did not answer the question for the CAN interfaces: ") +
                                     std::strerror(errno));
        }
        auto messages = parse_link_messages(std::span<const std::uint8_t>(buffer.data(), static_cast<std::size_t>(received)));
        for (auto& info : messages.interfaces)
        {
            result.push_back(std::move(info));
        }
        if (!messages.error.empty())
        {
            throw std::runtime_error("the kernel refused the question for the CAN interfaces: " + messages.error);
        }
        if (messages.done || received == 0)
        {
            break;
        }
    }
    return result;
}

} // namespace vehicle_sil::socketcan
