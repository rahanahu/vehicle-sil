// SPDX-License-Identifier: MIT
//
// TEST ONLY. The link factory of the bridge executable built for the process tests
// (vehicle_sil_ethernet_bridge_on_fake): fake links instead of the links of the operating system,
// chosen by --interface (any --backend):
//
//   fake0       a wire with one device (02:00:00:00:00:02) that answers every frame the bridge
//               sends: to the frame's source, from its own address, the bytes behind the two
//               addresses and the EtherType inverted
//   fake-fault  a link that reports a Fault as soon as it is started
//   other       no such interface (LinkError)
#include "fake_link.hpp"
#include "link_factory.hpp"

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace vehicle_sil::ethernet_bridge {
namespace {

using eth_link::LinkEvent;
using eth_link::LinkEventKind;
using fake_eth::FakeLink;
using fake_eth::FakeSegment;

constexpr std::uint8_t kDeviceMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};

/// The bridge's link on a wire of its own, and what else is on that wire.
class WiredFakeLink : public eth_link::ExternalLink
{
public:
    explicit WiredFakeLink(const std::string& name) : link_(segment_, name) {}

    void start(EventHandler handler) override { link_.start(std::move(handler)); }
    void stop() noexcept override { link_.stop(); }
    eth_link::TransmitResult transmit(std::span<const std::uint8_t> frame) override { return link_.transmit(frame); }
    [[nodiscard]] bool echoes_own_transmissions() const noexcept override { return link_.echoes_own_transmissions(); }
    [[nodiscard]] std::string describe() const override { return link_.describe(); }
    [[nodiscard]] std::string last_error() const override { return link_.last_error(); }

protected:
    FakeSegment segment_;
    FakeLink link_;
};

class AnsweringDeviceLink final : public WiredFakeLink
{
public:
    explicit AnsweringDeviceLink(const std::string& name) : WiredFakeLink(name), device_(segment_, "device") {}
    ~AnsweringDeviceLink() override { stop(); }

    void start(EventHandler handler) override
    {
        device_.start([this](const LinkEvent& event) {
            if (event.kind != LinkEventKind::Frame || event.frame.size() < eth_link::kEthernetHeaderBytes)
            {
                return;
            }
            std::vector<std::uint8_t> answer(event.frame.begin(), event.frame.end());
            for (std::size_t i = 0; i < 6; ++i)
            {
                answer[i] = event.frame[6 + i];
                answer[6 + i] = kDeviceMac[i];
            }
            for (std::size_t i = eth_link::kEthernetHeaderBytes; i < answer.size(); ++i)
            {
                answer[i] = static_cast<std::uint8_t>(~answer[i]);
            }
            (void)device_.transmit(answer);
        });
        try
        {
            WiredFakeLink::start(std::move(handler));
        }
        catch (...)
        {
            device_.stop();
            throw;
        }
    }
    void stop() noexcept override
    {
        WiredFakeLink::stop();
        device_.stop();
    }

private:
    FakeLink device_;
};

class FaultyLink final : public WiredFakeLink
{
public:
    using WiredFakeLink::WiredFakeLink;
    ~FaultyLink() override { stop(); }

    void start(EventHandler handler) override
    {
        WiredFakeLink::start(std::move(handler));
        link_.report_fault("interface 'fake-fault' is gone (fake)");
    }
};

/// Fails in start(), as a real link does: after the bridge joined SIL Kit.
class MissingLink final : public WiredFakeLink
{
public:
    explicit MissingLink(const std::string& name) : WiredFakeLink(name), name_(name) {}

    void start(EventHandler) override
    {
        throw eth_link::LinkError(eth_link::LinkErrorKind::NoSuchInterface,
                                  "no interface '" + name_ + "' (fake). Interfaces here: fake0, fake-fault.");
    }

private:
    const std::string name_;
};

} // namespace

std::unique_ptr<eth_link::ExternalLink> make_external_link(const BridgeOptions& options)
{
    if (options.interface_name == "fake0")
    {
        return std::make_unique<AnsweringDeviceLink>(options.interface_name);
    }
    if (options.interface_name == "fake-fault")
    {
        return std::make_unique<FaultyLink>(options.interface_name);
    }
    return std::make_unique<MissingLink>(options.interface_name);
}

std::string interface_list(const BridgeOptions&)
{
    return "fake interfaces:\n  --interface fake0\n  --interface fake-fault\n";
}

} // namespace vehicle_sil::ethernet_bridge
