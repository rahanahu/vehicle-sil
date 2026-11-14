// SPDX-License-Identifier: MIT
//
// Linux only. A network interface through a packet socket (AF_PACKET, SOCK_RAW) bound to it.
#include "linux_link.hpp"

#include "../os_link_errors.hpp"

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace vehicle_sil::eth_link {
namespace {

using namespace detail;

/// Destination and source MAC: a VLAN tag sits right behind them.
constexpr std::size_t kMacBytes = 12;
constexpr std::size_t kVlanTagBytes = 4;

class PacketLink final : public LinuxLink
{
public:
    explicit PacketLink(PacketLinkOptions options) : LinuxLink(options.name), options_(std::move(options)) {}
    ~PacketLink() override { stop(); }

    [[nodiscard]] std::string describe() const override { return "packet '" + name() + "' (network interface)"; }

protected:
    Fd open() override
    {
        const unsigned index = ::if_nametoindex(name().c_str());
        const auto state = interface_state(name());
        if (index == 0 || !state)
        {
            const auto names = interface_names();
            throw no_packet_interface(name(), names);
        }
        if (state->link_type != ARPHRD_ETHER)
        {
            throw not_ethernet(name(), state->link_type);
        }
        // Protocol 0: the socket gets nothing until bind() names the interface.
        Fd fd(::socket(AF_PACKET, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
        if (!fd.valid())
        {
            throw packet_open_failed(name(), errno, "socket", executable_path());
        }
        // A received frame's outer VLAN tag is not in its bytes: the kernel takes it out before
        // the socket sees the frame and tells it beside the frame (read_frame() puts it back).
        const int on = 1;
        if (::setsockopt(fd.get(), SOL_PACKET, PACKET_AUXDATA, &on, sizeof(on)) != 0)
        {
            throw packet_open_failed(name(), errno, "PACKET_AUXDATA", {});
        }
        sockaddr_ll address{};
        address.sll_family = AF_PACKET;
        address.sll_protocol = htons(ETH_P_ALL);
        address.sll_ifindex = static_cast<int>(index);
        if (::bind(fd.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0)
        {
            throw packet_open_failed(name(), errno, "bind", {});
        }
        if (options_.promiscuous)
        {
            // The socket's own membership: the kernel takes it back when the socket closes.
            packet_mreq membership{};
            membership.mr_ifindex = static_cast<int>(index);
            membership.mr_type = PACKET_MR_PROMISC;
            if (::setsockopt(fd.get(), SOL_PACKET, PACKET_ADD_MEMBERSHIP, &membership, sizeof(membership)) != 0)
            {
                throw packet_open_failed(name(), errno, "promiscuous mode", {});
            }
        }
        return fd;
    }

    ReadResult read_frame(int fd, std::span<std::uint8_t> buffer) override
    {
        // The frame is read behind room for one VLAN tag, so that the tag can be put back by
        // moving the two MAC addresses to the front.
        const std::span<std::uint8_t> room = buffer.subspan(kVlanTagBytes);
        iovec io{room.data(), room.size()};
        sockaddr_ll from{};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(tpacket_auxdata))];
        msghdr message{};
        message.msg_name = &from;
        message.msg_namelen = sizeof(from);
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);

        const ssize_t received = ::recvmsg(fd, &message, 0);
        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return {ReadStatus::Empty, {}, 0};
            }
            return {ReadStatus::Error, {}, errno};
        }
        // A copy of a frame this computer sent to a group address, handed up again for local
        // receivers: the socket has it already as the outgoing frame.
        if (from.sll_pkttype == PACKET_LOOPBACK)
        {
            return {ReadStatus::Skip, {}, 0};
        }
        const auto length = static_cast<std::size_t>(received);
        for (cmsghdr* header = CMSG_FIRSTHDR(&message); header != nullptr; header = CMSG_NXTHDR(&message, header))
        {
            if (header->cmsg_level != SOL_PACKET || header->cmsg_type != PACKET_AUXDATA ||
                header->cmsg_len < CMSG_LEN(sizeof(tpacket_auxdata)))
            {
                continue;
            }
            tpacket_auxdata aux{};
            std::memcpy(&aux, CMSG_DATA(header), sizeof(aux));
            const bool tagged = (aux.tp_status & TP_STATUS_VLAN_VALID) != 0 || aux.tp_vlan_tci != 0;
            if (!tagged || length < kMacBytes)
            {
                break;
            }
            const std::uint16_t tpid =
                (aux.tp_status & TP_STATUS_VLAN_TPID_VALID) != 0 ? aux.tp_vlan_tpid : static_cast<std::uint16_t>(ETH_P_8021Q);
            std::memmove(buffer.data(), room.data(), kMacBytes);
            buffer[kMacBytes] = static_cast<std::uint8_t>(tpid >> 8);
            buffer[kMacBytes + 1] = static_cast<std::uint8_t>(tpid & 0xFF);
            buffer[kMacBytes + 2] = static_cast<std::uint8_t>(aux.tp_vlan_tci >> 8);
            buffer[kMacBytes + 3] = static_cast<std::uint8_t>(aux.tp_vlan_tci & 0xFF);
            return {ReadStatus::Frame, buffer.first(length + kVlanTagBytes), 0};
        }
        return {ReadStatus::Frame, room.first(length), 0};
    }

    long write_frame(int fd, std::span<const std::uint8_t> frame) override
    {
        return static_cast<long>(::send(fd, frame.data(), frame.size(), MSG_DONTWAIT));
    }

    std::uint64_t take_overruns(int fd) noexcept override
    {
        // The kernel sets the counters back to 0 with each read.
        tpacket_stats statistics{};
        socklen_t size = sizeof(statistics);
        if (::getsockopt(fd, SOL_PACKET, PACKET_STATISTICS, &statistics, &size) != 0)
        {
            return 0;
        }
        return statistics.tp_drops;
    }

private:
    const PacketLinkOptions options_;
};

} // namespace

std::unique_ptr<ExternalLink> make_packet_link(PacketLinkOptions options)
{
    return std::make_unique<PacketLink>(std::move(options));
}

} // namespace vehicle_sil::eth_link
