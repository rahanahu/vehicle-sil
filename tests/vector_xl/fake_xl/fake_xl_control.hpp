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
#pragma once

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
