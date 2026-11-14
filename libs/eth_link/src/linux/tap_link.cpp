// SPDX-License-Identifier: MIT
//
// Linux only. A TAP device through /dev/net/tun: each read gives one frame the OS sent on the
// interface, each write hands the OS one frame as received on it.
#include "linux_link.hpp"

#include "../os_link_errors.hpp"

#include <net/if.h>

#include <fcntl.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

namespace vehicle_sil::eth_link {
namespace {

using namespace detail;

class TapLink final : public LinuxLink
{
public:
    explicit TapLink(TapLinkOptions options) : LinuxLink(options.name), options_(std::move(options)) {}
    ~TapLink() override { stop(); }

    [[nodiscard]] std::string describe() const override
    {
        return "tap '" + name() + "' (TAP device in this computer" + (options_.create ? ", made by the bridge)" : ")");
    }

protected:
    Fd open() override
    {
        const bool exists = ::if_nametoindex(name().c_str()) != 0;
        if (options_.create && exists)
        {
            throw tap_exists(name());
        }
        if (!options_.create && !exists)
        {
            const auto names = interface_names();
            throw no_tap_device(name(), names);
        }
        Fd fd(::open("/dev/net/tun", O_RDWR | O_CLOEXEC | O_NONBLOCK));
        if (!fd.valid())
        {
            throw tun_open_failed(errno);
        }
        ifreq request{};
        std::memcpy(request.ifr_name, name().data(), name().size());
        // Frames with their Ethernet header and nothing in front. Creating: fail instead of
        // attaching if the name was taken meanwhile.
        request.ifr_flags = static_cast<short>(IFF_TAP | IFF_NO_PI | (options_.create ? IFF_TUN_EXCL : 0));
        if (::ioctl(fd.get(), TUNSETIFF, &request) != 0)
        {
            const int error = errno;
            if (options_.create)
            {
                throw error == EBUSY ? tap_exists(name()) : tap_create_failed(name(), error);
            }
            throw tap_attach_failed(name(), error);
        }
        return fd;
    }

    ReadResult read_frame(int fd, std::span<std::uint8_t> buffer) override
    {
        const ssize_t length = ::read(fd, buffer.data(), buffer.size());
        if (length > 0)
        {
            return {ReadStatus::Frame, buffer.first(static_cast<std::size_t>(length)), 0};
        }
        if (length == 0)
        {
            return {ReadStatus::Skip, {}, 0};
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return {ReadStatus::Empty, {}, 0};
        }
        return {ReadStatus::Error, {}, errno};
    }

    long write_frame(int fd, std::span<const std::uint8_t> frame) override
    {
        return static_cast<long>(::write(fd, frame.data(), frame.size()));
    }

private:
    const TapLinkOptions options_;
};

} // namespace

std::unique_ptr<ExternalLink> make_tap_link(TapLinkOptions options)
{
    return std::make_unique<TapLink>(std::move(options));
}

} // namespace vehicle_sil::eth_link
