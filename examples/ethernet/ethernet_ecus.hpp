// SPDX-License-Identifier: MIT
//
// Two ECUs that exchange raw Ethernet frames (vehicle_sil::Ecu::ethernet()): a sensor that sends
// a reading every 100 ms and a display that shows it and acknowledges each one. The frames are
// built and read here, byte by byte: vehicle-sil carries them and has no protocol stack.
//
// The values of this example are the application's own, and you can change them:
// - The EtherType (Options::ether_type, 0x88B5 by default) is one that is commonly used for local
//   experiments. vehicle-sil does not read it, and the value was not checked against the standard.
// - The MAC addresses are locally administered ones (the bit 0x02 of the first byte is set), so
//   they are not addresses of a real manufacturer.
#pragma once

#include "vehicle_sil/ecu/ecu.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>

namespace ethernet_example {

/// The Ethernet network both ECUs are on (simulation.yaml: kind: ethernet).
inline constexpr const char* kNetwork = "ETH";

inline constexpr vehicle_sil::eth::MacAddress kSensorMac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
inline constexpr vehicle_sil::eth::MacAddress kDisplayMac{{0x02, 0x00, 0x00, 0x00, 0x00, 0x02}};

struct Options
{
    /// Bytes 12 and 13 of every frame of this example; frames with another value are ignored.
    std::uint16_t ether_type = 0x88B5;
};

/// The first byte of the payload.
enum class Kind : std::uint8_t
{
    Reading = 1, ///< sensor -> display: counter (4 bytes, big endian), value (2 bytes, big endian)
    Ack = 2,     ///< display -> sensor: the counter of the reading
};

inline constexpr std::size_t kPayloadAt = vehicle_sil::eth::kHeaderBytes;
inline constexpr std::size_t kReadingBytes = kPayloadAt + 7;
inline constexpr std::size_t kAckBytes = kPayloadAt + 5;

/// The header of a frame from `from` to `to`, and the kind and the counter of its payload. The
/// rest of `bytes` stays as it is.
inline void write_start(std::span<std::uint8_t> bytes, const vehicle_sil::eth::MacAddress& to,
                        const vehicle_sil::eth::MacAddress& from, std::uint16_t ether_type, Kind kind,
                        std::uint32_t counter)
{
    std::copy(to.bytes.begin(), to.bytes.end(), bytes.begin());
    std::copy(from.bytes.begin(), from.bytes.end(), bytes.begin() + 6);
    bytes[12] = static_cast<std::uint8_t>(ether_type >> 8);
    bytes[13] = static_cast<std::uint8_t>(ether_type & 0xFF);
    bytes[kPayloadAt] = static_cast<std::uint8_t>(kind);
    for (std::size_t i = 0; i < 4; ++i)
    {
        bytes[kPayloadAt + 1 + i] = static_cast<std::uint8_t>(counter >> (8 * (3 - i)));
    }
}

/// true if `bytes` is a frame of this example of the kind `kind` (the networks pad short frames
/// with zero bytes, so it may be longer than what was sent).
inline bool is(std::span<const std::uint8_t> bytes, std::uint16_t ether_type, Kind kind, std::size_t length)
{
    return bytes.size() >= length && bytes[12] == (ether_type >> 8) && bytes[13] == (ether_type & 0xFF) &&
           bytes[kPayloadAt] == static_cast<std::uint8_t>(kind);
}

inline std::uint32_t counter_of(std::span<const std::uint8_t> bytes)
{
    std::uint32_t counter = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        counter = (counter << 8) | bytes[kPayloadAt + 1 + i];
    }
    return counter;
}

/// Sends its value to the display every 100 ms and counts the acknowledgements.
///
/// Variables: sensor.value (input: what it measures), sensor.sent, sensor.acked (outputs).
class Sensor
{
public:
    explicit Sensor(vehicle_sil::Ecu& ecu, Options options = {}) :
      channel_(ecu.ethernet(kNetwork, {.mac = kSensorMac})), options_(options)
    {
        ecu.input("sensor.value", &value_, {.min = 0.0, .max = 65535.0, .description = "what the sensor measures"});
        ecu.output("sensor.sent", &sent_, {.description = "readings sent"});
        ecu.output("sensor.acked", &acked_, {.description = "readings the display acknowledged"});
        // A handler runs at the start of a step, for each frame that arrived since the last one.
        channel_.on_frame([this](const vehicle_sil::eth::TimedFrame& received) {
            const auto bytes = received.frame.bytes();
            if (received.frame.destination() == kSensorMac && is(bytes, options_.ether_type, Kind::Ack, kAckBytes))
            {
                ++acked_;
                last_acked_ = counter_of(bytes);
                last_ack_time_ = received.time;
            }
        });
        // send() works in a step; the frame leaves at the end of this step.
        ecu.every(std::chrono::milliseconds(100), [this](vehicle_sil::SimTime) {
            std::array<std::uint8_t, kReadingBytes> bytes{};
            write_start(bytes, kDisplayMac, kSensorMac, options_.ether_type, Kind::Reading, static_cast<std::uint32_t>(sent_));
            bytes[kPayloadAt + 5] = static_cast<std::uint8_t>(value_ >> 8);
            bytes[kPayloadAt + 6] = static_cast<std::uint8_t>(value_ & 0xFF);
            if (channel_.send(bytes) == vehicle_sil::eth::SendStatus::Ok)
            {
                ++sent_;
            }
        });
    }

    [[nodiscard]] int sent() const noexcept { return sent_; }
    [[nodiscard]] int acked() const noexcept { return acked_; }
    [[nodiscard]] std::uint32_t last_acked() const noexcept { return last_acked_; }
    /// When the display sent the last acknowledgement.
    [[nodiscard]] vehicle_sil::SimTime last_ack_time() const noexcept { return last_ack_time_; }
    [[nodiscard]] const vehicle_sil::eth::Channel& channel() const noexcept { return channel_; }

private:
    vehicle_sil::eth::Channel& channel_;
    Options options_;
    int value_ = 20;
    int sent_ = 0;
    int acked_ = 0;
    std::uint32_t last_acked_ = 0;
    vehicle_sil::SimTime last_ack_time_{};
};

/// Shows the last reading and acknowledges each one. It takes only the frames addressed to it
/// (eth::Accept::Addressed); without that a channel sees every frame on the network.
///
/// Variables: display.value, display.received (outputs).
class Display
{
public:
    explicit Display(vehicle_sil::Ecu& ecu, Options options = {}) :
      channel_(ecu.ethernet(kNetwork, {.mac = kDisplayMac, .accept = vehicle_sil::eth::Accept::Addressed})),
      options_(options)
    {
        ecu.output("display.value", &value_, {.description = "the reading shown"});
        ecu.output("display.received", &received_, {.description = "readings received"});
        // The other way to receive: take the frames of this step, one by one, from the step.
        ecu.on_step([this](vehicle_sil::SimTime) {
            while (const auto received = channel_.receive())
            {
                const auto bytes = received->frame.bytes();
                if (!is(bytes, options_.ether_type, Kind::Reading, kReadingBytes))
                {
                    continue;
                }
                value_ = (bytes[kPayloadAt + 5] << 8) | bytes[kPayloadAt + 6];
                ++received_;
                last_reading_time_ = received->time;
                std::array<std::uint8_t, kAckBytes> ack{};
                write_start(ack, received->frame.source(), kDisplayMac, options_.ether_type, Kind::Ack, counter_of(bytes));
                static_cast<void>(channel_.send(ack));
            }
        });
    }

    [[nodiscard]] int value() const noexcept { return value_; }
    [[nodiscard]] int received() const noexcept { return received_; }
    /// When the sensor sent the last reading.
    [[nodiscard]] vehicle_sil::SimTime last_reading_time() const noexcept { return last_reading_time_; }
    [[nodiscard]] const vehicle_sil::eth::Channel& channel() const noexcept { return channel_; }

private:
    vehicle_sil::eth::Channel& channel_;
    Options options_;
    int value_ = 0;
    int received_ = 0;
    vehicle_sil::SimTime last_reading_time_{};
};

} // namespace ethernet_example
