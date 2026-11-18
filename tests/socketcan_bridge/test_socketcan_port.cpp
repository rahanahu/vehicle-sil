// SPDX-License-Identifier: MIT
#include "fake_can_device.hpp"
#include "test_harness.hpp"

#include "vehicle_sil/socketcan/error_frame.hpp"
#include "vehicle_sil/socketcan/socketcan_port.hpp"

#include <linux/can/error.h>

#include <cerrno>
#include <chrono>
#include <mutex>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::socketcan;
using vehicle_sil::can_bridge::ExternalBusState;
using vehicle_sil::can_bridge::ExternalCanEvent;
using vehicle_sil::can_bridge::ExternalEventKind;
using vehicle_sil::can_bridge::TransmitResult;
using vehicle_sil::unit::eventually;
using vehicle_sil::unit::FakeCanDevice;
namespace can = SilKit::Services::Can;

constexpr std::uint32_t kExtended = 0x80000000u;
constexpr std::uint32_t kRemote = 0x40000000u;

struct Seen
{
    ExternalEventKind kind;
    std::uint32_t id = 0;
    can::CanFrameFlagMask flags = 0;
    std::uint16_t dlc = 0;
    std::vector<std::uint8_t> data;
    ExternalBusState bus;
    std::string detail;
};

/// A port on a fake device, with its events collected.
class Rig
{
public:
    explicit Rig(SocketCanPortOptions options = {}) : port(device, with_short_watch(options)) {}
    ~Rig() { port.stop(); }

    void start()
    {
        port.start([this](const ExternalCanEvent& event) {
            Seen seen{event.kind, event.frame.canId, event.frame.flags, event.frame.dlc,
                      std::vector<std::uint8_t>(event.frame.dataField.begin(), event.frame.dataField.end()), event.bus_state,
                      std::string(event.detail)};
            const std::lock_guard lock(mutex);
            events.push_back(std::move(seen));
        });
    }

    std::vector<Seen> seen() const
    {
        const std::lock_guard lock(mutex);
        return events;
    }
    std::size_t count(ExternalEventKind kind) const
    {
        std::size_t n = 0;
        for (const auto& e : seen())
        {
            n += e.kind == kind ? 1 : 0;
        }
        return n;
    }
    bool wait_for(ExternalEventKind kind, std::size_t n = 1)
    {
        return eventually([&] { return count(kind) >= n; }, 5000ms, 2ms);
    }
    /// The first event of `kind` after the `skip` before it.
    std::optional<Seen> first(ExternalEventKind kind) const
    {
        for (const auto& e : seen())
        {
            if (e.kind == kind)
            {
                return e;
            }
        }
        return std::nullopt;
    }
    bool has_warning(const std::string& part) const
    {
        for (const auto& e : seen())
        {
            if (e.kind == ExternalEventKind::Warning && e.detail.find(part) != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }

    FakeCanDevice device;
    SocketCanPort port;
    mutable std::mutex mutex;
    std::vector<Seen> events;

private:
    static SocketCanPortOptions with_short_watch(SocketCanPortOptions options)
    {
        options.watch_interval = 10ms;
        return options;
    }
};

RawFrame frame(std::uint32_t id, std::initializer_list<std::uint8_t> bytes, bool confirm = false)
{
    RawFrame raw;
    raw.can_id = id;
    raw.length = static_cast<std::uint8_t>(bytes.size());
    std::copy(bytes.begin(), bytes.end(), raw.data.begin());
    raw.confirm = confirm;
    raw.sent_here = confirm;
    return raw;
}

can::CanFrame silkit_frame(std::uint32_t id, can::CanFrameFlagMask flags, std::uint16_t dlc, const std::vector<std::uint8_t>& data)
{
    can::CanFrame f{};
    f.canId = id;
    f.flags = flags;
    f.dlc = dlc;
    f.dataField = SilKit::Util::Span<const std::uint8_t>(data.data(), data.size());
    return f;
}

constexpr can::CanFrameFlagMask kIde = static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Ide);
constexpr can::CanFrameFlagMask kRtr = static_cast<can::CanFrameFlagMask>(can::CanFrameFlag::Rtr);

} // namespace

TEST_CASE(port_start_failure_throws_and_leaves_nothing_running)
{
    Rig rig;
    rig.device.fail_open = true;
    CHECK(vehicle_sil::unit::throws_with<std::runtime_error>([&] { rig.start(); }, "cannot open the fake interface"));
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
}

TEST_CASE(port_frames_from_devices_and_receipts_of_its_own)
{
    Rig rig;
    rig.start();
    rig.device.push_frame(frame(0x123, {1, 2, 3}));
    rig.device.push_frame(frame(0x124, {9}, /*confirm=*/true));
    // Another program of this computer: sent here, not by this socket. It is a frame from a device.
    auto local = frame(0x125, {7});
    local.sent_here = true;
    rig.device.push_frame(local);
    CHECK(rig.wait_for(ExternalEventKind::Frame, 2));
    CHECK(rig.wait_for(ExternalEventKind::OwnTransmission));
    const auto events = rig.seen();
    CHECK_EQ(events.size(), std::size_t{3});
    CHECK(events[0].kind == ExternalEventKind::Frame && events[0].id == 0x123 && events[0].dlc == 3 && events[0].flags == 0);
    CHECK((events[0].data == std::vector<std::uint8_t>{1, 2, 3}));
    CHECK(events[1].kind == ExternalEventKind::OwnTransmission && events[1].id == 0x124);
    CHECK(events[2].kind == ExternalEventKind::Frame && events[2].id == 0x125);
}

TEST_CASE(port_extended_and_remote_frames)
{
    Rig rig;
    rig.start();
    RawFrame extended = frame(0x18FF1080 | kExtended, {0xAA, 0xBB});
    RawFrame remote = frame(0x321 | kRemote, {});
    remote.length = 4; // the requested length; no data
    rig.device.push_frame(extended);
    rig.device.push_frame(remote);
    CHECK(rig.wait_for(ExternalEventKind::Frame, 2));
    const auto events = rig.seen();
    CHECK(events[0].id == 0x18FF1080u && events[0].flags == kIde);
    CHECK(events[1].id == 0x321u && events[1].flags == kRtr && events[1].dlc == 4 && events[1].data.empty());
}

TEST_CASE(port_transmit_maps_the_frame)
{
    Rig rig;
    rig.start();
    CHECK(rig.port.transmit(silkit_frame(0x18EAFF00, kIde | kRtr, 3, {})) == TransmitResult::Ok);
    CHECK(rig.port.transmit(silkit_frame(0x7FF, 0, 2, {5, 6})) == TransmitResult::Ok);
    // DLC 9..15 means 8 data bytes on classic CAN, and goes out as 8.
    CHECK(rig.port.transmit(silkit_frame(0x456, 0, 15, {1, 2, 3, 4, 5, 6, 7, 8})) == TransmitResult::Ok);
    const auto written = rig.device.written();
    CHECK_EQ(written.size(), std::size_t{3});
    CHECK(written[0].can_id == (0x18EAFF00u | kExtended | kRemote) && written[0].length == 3);
    CHECK(written[1].can_id == 0x7FFu && written[1].length == 2 && written[1].data[0] == 5 && written[1].data[1] == 6);
    CHECK(written[2].can_id == 0x456u && written[2].length == 8 && written[2].data[7] == 8);
}

TEST_CASE(port_full_queue_is_queue_full)
{
    Rig rig;
    rig.start();
    rig.device.set_write_error(ENOBUFS);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::QueueFull);
    rig.device.set_write_error(EAGAIN);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::QueueFull);
    rig.device.set_write_error(0);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Ok);
    CHECK(rig.port.last_error().empty());
}

TEST_CASE(port_other_write_errors_fail_and_say_why)
{
    Rig rig;
    rig.start();
    rig.device.set_write_error(EINVAL);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
    CHECK(rig.port.last_error().find("sending failed: Invalid argument") != std::string::npos);
    CHECK_EQ(rig.count(ExternalEventKind::Fault), std::size_t{0});
}

TEST_CASE(port_error_frames_set_the_bus_state)
{
    Rig rig;
    rig.start();
    rig.device.push_frame(frame(kErrorFlag | CAN_ERR_CRTL | CAN_ERR_CNT, {0, CAN_ERR_CRTL_TX_PASSIVE, 0, 0, 0, 0, 128, 0}));
    rig.device.push_frame(frame(kErrorFlag | CAN_ERR_BUSOFF, {}));
    rig.device.push_frame(frame(kErrorFlag | CAN_ERR_BUSERROR, {}));
    rig.device.push_frame(frame(kErrorFlag | CAN_ERR_RESTARTED, {}));
    CHECK(rig.wait_for(ExternalEventKind::ErrorFrame, 4));
    CHECK(rig.wait_for(ExternalEventKind::BusState, 3));
    std::vector<ExternalBusState::Level> levels;
    for (const auto& e : rig.seen())
    {
        if (e.kind == ExternalEventKind::BusState)
        {
            levels.push_back(e.bus.level);
        }
    }
    using Level = ExternalBusState::Level;
    CHECK((levels == std::vector<Level>{Level::ErrorPassive, Level::BusOff, Level::ErrorActive}));
    // The error frames are not frames: nothing goes to SIL Kit for them.
    CHECK_EQ(rig.count(ExternalEventKind::Frame), std::size_t{0});
    CHECK(rig.first(ExternalEventKind::BusState)->bus.tx_errors == 128);
}

TEST_CASE(port_reads_the_controller_state_of_an_adapter)
{
    Rig rig;
    CanInterfaceInfo info;
    info.name = "can0";
    info.index = 3;
    info.up = true;
    info.bitrate = 250000;
    info.state = "bus-off";
    rig.device.set_interface(info);
    rig.start();
    CHECK(rig.wait_for(ExternalEventKind::BusState));
    CHECK(rig.first(ExternalEventKind::BusState)->bus.level == ExternalBusState::Level::BusOff);
    info.state = "error-active";
    rig.device.set_interface(info);
    CHECK(eventually([&] { return rig.count(ExternalEventKind::BusState) >= 2; }, 5000ms, 2ms));
}

TEST_CASE(port_vcan_has_no_bus_state)
{
    Rig rig;
    rig.start();
    std::this_thread::sleep_for(100ms);
    CHECK_EQ(rig.count(ExternalEventKind::BusState), std::size_t{0});
    CHECK(rig.port.link_state() == LinkState::Up);
}

TEST_CASE(port_interface_down_and_up_again)
{
    Rig rig;
    rig.start();
    CHECK(eventually([&] { return rig.port.link_state() == LinkState::Up; }, 5000ms, 2ms));
    rig.device.set_up(false);
    CHECK(eventually([&] { return rig.port.link_state() == LinkState::Down; }, 5000ms, 2ms));
    CHECK(rig.wait_for(ExternalEventKind::Warning));
    CHECK(rig.has_warning("interface vcan0 is down: frames are not sent or received until it is up again (sudo ip link set vcan0 up)"));
    rig.device.set_up(true);
    CHECK(eventually([&] { return rig.port.link_state() == LinkState::Up; }, 5000ms, 2ms));
    CHECK(eventually([&] { return rig.has_warning("interface vcan0 is up again"); }, 5000ms, 2ms));
    CHECK_EQ(rig.count(ExternalEventKind::Fault), std::size_t{0});
}

TEST_CASE(port_send_to_an_interface_that_is_down_is_a_failure_and_a_link_down)
{
    // A watch interval of a minute: only the send makes the thread look.
    SocketCanPortOptions options;
    options.watch_interval = 60s;
    FakeCanDevice device;
    SocketCanPort port(device, options);
    std::mutex mutex;
    std::vector<std::string> warnings;
    port.start([&](const ExternalCanEvent& event) {
        const std::lock_guard lock(mutex);
        if (event.kind == ExternalEventKind::Warning)
        {
            warnings.emplace_back(event.detail);
        }
    });
    CHECK(eventually([&] { return port.link_state() == LinkState::Up; }, 5000ms, 2ms));
    device.set_up(false);
    device.set_write_error(ENETDOWN);
    CHECK(port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
    CHECK(port.last_error().find("the interface is down") != std::string::npos);
    CHECK(eventually([&] { return port.link_state() == LinkState::Down; }, 5000ms, 2ms));
    CHECK(eventually([&] {
        const std::lock_guard lock(mutex);
        return !warnings.empty();
    }, 5000ms, 2ms));
    port.stop();
}

TEST_CASE(port_read_error_enetdown_looks_at_the_interface)
{
    Rig rig;
    rig.start();
    CHECK(eventually([&] { return rig.port.link_state() == LinkState::Up; }, 5000ms, 2ms));
    rig.device.set_up(false);
    rig.device.push_error(ENETDOWN);
    CHECK(eventually([&] { return rig.port.link_state() == LinkState::Down; }, 5000ms, 2ms));
    CHECK_EQ(rig.count(ExternalEventKind::Fault), std::size_t{0});
}

TEST_CASE(port_interface_gone_on_read_is_a_fault)
{
    Rig rig;
    rig.start();
    rig.device.push_error(ENODEV);
    CHECK(rig.wait_for(ExternalEventKind::Fault));
    CHECK(rig.first(ExternalEventKind::Fault)->detail.find("the interface is gone") != std::string::npos);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
    CHECK(rig.port.last_error().find("the interface is gone") != std::string::npos);
}

TEST_CASE(port_enxio_is_a_fault_too)
{
    Rig rig;
    rig.start();
    rig.device.push_error(ENXIO);
    CHECK(rig.wait_for(ExternalEventKind::Fault));
}

TEST_CASE(port_interface_gone_on_send_is_a_fault)
{
    Rig rig;
    rig.start();
    rig.device.set_write_error(ENODEV);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
    CHECK(rig.wait_for(ExternalEventKind::Fault));
    CHECK_EQ(rig.count(ExternalEventKind::Fault), std::size_t{1});
}

TEST_CASE(port_interface_removed_or_renamed_is_found_by_the_look)
{
    Rig rig;
    rig.start();
    rig.device.set_interface(std::nullopt);
    CHECK(rig.wait_for(ExternalEventKind::Fault));
    CHECK(rig.first(ExternalEventKind::Fault)->detail.find("removed or renamed") != std::string::npos);
}

TEST_CASE(port_other_read_errors_are_faults_and_transient_ones_are_not)
{
    Rig rig;
    rig.start();
    rig.device.push_error(ENOBUFS);
    rig.device.push_error(EINTR);
    rig.device.push_frame(frame(0x10, {1}));
    CHECK(rig.wait_for(ExternalEventKind::Frame));
    CHECK_EQ(rig.count(ExternalEventKind::Fault), std::size_t{0});
    rig.device.push_error(EIO);
    CHECK(rig.wait_for(ExternalEventKind::Fault));
}

TEST_CASE(port_transmit_timeout_drops_while_the_bus_takes_nothing)
{
    SocketCanPortOptions options;
    options.transmit_timeout = 100ms;
    Rig rig(options);
    rig.start();
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Ok);
    std::this_thread::sleep_for(150ms);
    // No receipt for longer than the timeout: the next frame is not sent... except as the probe that
    // goes out once the timeout has passed since the last one.
    CHECK(rig.port.transmit(silkit_frame(2, 0, 0, {})) == TransmitResult::Ok); // the probe
    CHECK(rig.port.transmit(silkit_frame(3, 0, 0, {})) == TransmitResult::Dropped);
    CHECK(rig.port.transmit(silkit_frame(4, 0, 0, {})) == TransmitResult::Dropped);
    CHECK_EQ(rig.device.written().size(), std::size_t{2});
    // A receipt: the bus works again.
    rig.device.push_frame(frame(2, {}, /*confirm=*/true));
    CHECK(rig.wait_for(ExternalEventKind::OwnTransmission));
    CHECK(rig.port.transmit(silkit_frame(5, 0, 0, {})) == TransmitResult::Ok);
    CHECK(rig.port.transmit(silkit_frame(6, 0, 0, {})) == TransmitResult::Ok);
}

TEST_CASE(port_without_a_transmit_timeout_frames_wait)
{
    Rig rig;
    rig.start();
    for (int i = 0; i < 20; ++i)
    {
        CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Ok);
    }
    std::this_thread::sleep_for(50ms);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Ok);
}

TEST_CASE(port_stop_closes_the_device_and_no_event_follows)
{
    Rig rig;
    rig.start();
    rig.port.stop();
    CHECK(rig.device.closed());
    const auto before = rig.seen().size();
    rig.device.push_frame(frame(0x1, {1}));
    std::this_thread::sleep_for(50ms);
    CHECK_EQ(rig.seen().size(), before);
    CHECK(rig.port.transmit(silkit_frame(1, 0, 0, {})) == TransmitResult::Failed);
    rig.port.stop(); // twice is fine
}

TEST_CASE(port_can_start_again_after_a_stop)
{
    Rig rig;
    rig.start();
    rig.port.stop();
    rig.start();
    rig.device.push_frame(frame(0x2, {2}));
    CHECK(rig.wait_for(ExternalEventKind::Frame));
}
