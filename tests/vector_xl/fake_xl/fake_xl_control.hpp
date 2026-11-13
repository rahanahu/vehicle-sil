// SPDX-License-Identifier: MIT
//
// TEST-ONLY control interface of the fake XL backend (fake_xl_driver.cpp), which implements the
// internal driver interface bridges/vector-xl/src/xl_backend.hpp in process.
//
// Modelled behaviour (what the XL backend logic relies on):
//  - Each channel is its own CAN bus. A frame transmitted through a port reaches every other
//    active port and TestNode on that channel as a plain received frame.
//  - A new port gets transmit receipts of its own frames (transmit_receipt) until
//    set_transmit_receipts(false, ...) is called; requests=true adds transmit_request events.
//    set_transmit_receipts and set_bitrate need init access (kStatusNoAccess otherwise).
//  - The first port that requests init access to a channel gets it; later ones get none.
//  - Opening a port on a channel that cannot do CAN fails with kStatusNoAccess.
//  - A receive queue holds rx_queue_size - 1 events; on overflow the event is dropped and the
//    next one that is queued carries queue_overrun.
//  - transmit accepts DLC 0..8 only (kStatusBadArgument otherwise). receive returns one event
//    per call.
//  - Environment (read by open_driver, for process level tests):
//      VEHICLE_SIL_FAKE_XL_FAIL_OPEN=1   open_driver fails with kStatusDriverUnavailable
//      VEHICLE_SIL_FAKE_XL_IGNORE_CHANNEL_MODE=1  like set_ignore_channel_mode(true)
//      VEHICLE_SIL_FAKE_XL_ECHO=<index>  an echo node on that channel answers every data frame
//                                        with the same identifier and inverted data bytes, and
//                                        every remote frame with a data frame of DLC bytes 0,1,2...
//      VEHICLE_SIL_FAKE_XL_LOSE_DEVICE_AFTER_MS=<ms>  like set_device_lost(true), <ms> after open_driver
//  - request_chip_state on an active port queues a chip_state event with the channel's state
//    (set_chip_state; error active, 0/0 by default); kStatusNotOnBus on an inactive port,
//    kStatusQueueFull while set_transmit_queue_full, kStatusNoAccess while
//    set_chip_state_refused.
//  - While the device is lost, request_chip_state and transmit fail with kStatusDeviceLost and
//    nothing is received.
//  - While a channel's bus is blocked (set_bus_blocked; no node to acknowledge), transmit
//    accepts frames into the port's transmit queue without sending them; unblocking sends them
//    in order (with their receipts). deactivate discards the queue.
#pragma once

#include "xl_backend.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fake_xl {

/// Status codes of the fake (the `code` of a backend status, XlError::status()).
inline constexpr int kStatusOk = 0;
inline constexpr int kStatusNothingToReceive = 1;
inline constexpr int kStatusQueueFull = 2;
inline constexpr int kStatusBadArgument = 3;
inline constexpr int kStatusNoAccess = 4;
inline constexpr int kStatusDriverUnavailable = 5;
inline constexpr int kStatusNotOnBus = 6;
inline constexpr int kStatusDeviceLost = 7;

struct ChannelDef
{
    std::string name;
    unsigned hw_type = 0; ///< passed through as XlChannelInfo::hw_type
    bool is_virtual = true;
    bool can_capable = true;
    unsigned hw_index = 0;
    unsigned hw_channel = 0;
    unsigned serial = 0;
    std::string transceiver;
};

/// Hardware type numbers the default channels report. Arbitrary test values.
inline constexpr unsigned kVirtualHwType = 100;
inline constexpr unsigned kDeviceHwType = 200;

/// Default configuration: 0 "Virtual Channel 1", 1 "Virtual Channel 2" (virtual CAN),
/// 2 "Test CAN Device Channel 1", 3 "Test CAN Device Channel 2" (hardware CAN), 4 "Test CAN Device Channel 3" (LIN only).
std::vector<ChannelDef> default_channels();

/// Closes all ports, removes test nodes, restores default channels and options.
void reset();
void set_channels(std::vector<ChannelDef> channels);
void set_fail_open_driver(bool fail);
/// transmit fails with kStatusQueueFull while set.
void set_transmit_queue_full(bool full);
/// The driver keeps sending transmit receipts even after set_transmit_receipts(false, false).
void set_ignore_channel_mode(bool ignore);
/// Every port on `channel_index` reports queue_overrun with its next event.
void inject_overrun(unsigned channel_index);
/// The state request_chip_state reports for `channel_index` from now on.
void set_chip_state(unsigned channel_index, vehicle_sil::vector_xl::detail::backend::ChipState state);
/// The hardware disappears (true) or comes back (false) for every channel.
void set_device_lost(bool lost);
/// request_chip_state fails with kStatusNoAccess while set (a setup without the query).
void set_chip_state_refused(bool refused);
/// activate fails with kStatusDeviceLost while set.
void set_activate_fails(bool fails);
/// Activating a port on `channel_index` ends its bus off (a reset controller on a repaired bus)
/// unless this is set (the bus is still broken).
void set_bus_off_persists(unsigned channel_index, bool persists);
/// How many times a port on `channel_index` was activated since reset().
unsigned activations(unsigned channel_index);
/// How many times request_chip_state was called (any port).
unsigned chip_state_requests();
/// Blocks (true) or unblocks (false) the bus of `channel_index`; see the modelled behaviour.
void set_bus_blocked(unsigned channel_index, bool blocked);
/// Frames waiting in the transmit queues of the ports on `channel_index`.
std::size_t queued_transmissions(unsigned channel_index);
/// The next transmit receipt on `channel_index` is not delivered (lost).
void lose_next_receipt(unsigned channel_index);
/// The waiting frames on `channel_index` disappear without being sent or confirmed (as a driver
/// might after bus off).
void discard_queued_transmissions(unsigned channel_index);

struct Frame
{
    std::uint32_t id = 0;
    bool extended = false;
    bool remote = false;
    std::uint8_t dlc = 0;
    std::vector<std::uint8_t> data;
    bool operator==(const Frame&) const = default;
};

struct PortState
{
    bool open = false;
    bool active = false;
    bool init_access = false;
    bool tx_receipts = false;
    std::optional<unsigned long> bitrate;
};
/// State of the (first) port open on `channel_index`.
PortState port_state(unsigned channel_index);

/// Another application on the bus of one channel.
class TestNode
{
public:
    explicit TestNode(unsigned channel_index);
    ~TestNode();
    TestNode(const TestNode&) = delete;
    TestNode& operator=(const TestNode&) = delete;

    void send(const Frame& frame);
    void send_error_frame();
    /// Next frame another node transmitted on this bus, or nullopt after `timeout`.
    std::optional<Frame> receive(std::chrono::milliseconds timeout);

private:
    int id_;
};

} // namespace fake_xl
