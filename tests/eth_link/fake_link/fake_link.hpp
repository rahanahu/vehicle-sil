// SPDX-License-Identifier: MIT
//
// TEST ONLY. An in-memory ExternalLink with a receive thread, and the wire it is attached to.
//
//   test ("a device") --send()--> FakeSegment --> every started FakeLink on it --> its handler
//   FakeLink::transmit() -------> FakeSegment --> every other started FakeLink (and itself
//                                                 with set_echo_own(true); no link of
//                                                 vehicle-sil does today)
//
// Each link delivers its events from its own thread, in the order they were queued, like a
// backend's receive thread. Frames queued while a link is not started are dropped, as an
// interface nobody listens on does.
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vehicle_sil::fake_eth {

using Bytes = std::vector<std::uint8_t>;

class FakeLink;

/// The wire between fake links and the test.
class FakeSegment
{
public:
    /// A device on the wire sends `frame`: every started link receives it.
    void send(std::span<const std::uint8_t> frame);
    /// The frames the links transmitted, in order (what a device on the wire received).
    [[nodiscard]] std::vector<Bytes> transmitted() const;
    /// No link has an event queued or is running its handler.
    [[nodiscard]] bool idle() const;

private:
    friend class FakeLink;
    void attach(FakeLink& link);
    void detach(FakeLink& link);
    void transmit_from(const FakeLink& sender, std::span<const std::uint8_t> frame, bool echo_own);

    mutable std::mutex mutex_;
    std::vector<FakeLink*> links_;
    std::vector<Bytes> transmitted_;
};

class FakeLink final : public eth_link::ExternalLink
{
public:
    FakeLink(FakeSegment& segment, std::string name);
    ~FakeLink() override;

    FakeLink(const FakeLink&) = delete;
    FakeLink& operator=(const FakeLink&) = delete;

    // --- test controls
    /// start() throws LinkError(kind, message).
    void fail_start(eth_link::LinkErrorKind kind, std::string message);
    /// What transmit() returns; anything but Ok sends nothing.
    void set_transmit_result(eth_link::TransmitResult result);
    /// Transmitted frames come back to this link's own handler too (no link of vehicle-sil does
    /// this today), and echoes_own_transmissions() says so. Set it before start().
    void set_echo_own(bool echo);
    /// transmit() throws std::runtime_error (a link that breaks its contract).
    void set_transmit_throws(bool throws);
    /// A frame that reaches this link only (a device in front of this interface alone).
    void receive(std::span<const std::uint8_t> frame);
    /// As receive(), with LinkEvent::checksum_incomplete set (a frame of this computer's OS whose
    /// checksum is left to the network adapter). The Warning of a real link is not made up here.
    void receive_checksum_incomplete(std::span<const std::uint8_t> frame);
    /// stop() reports this many frames lost by the OS before it returns.
    void set_overruns_at_stop(std::uint64_t count);
    /// Queues an event for the receive thread.
    void report_overrun(std::uint64_t count);
    void report_link_state(eth_link::LinkState state);
    void report_warning(std::string detail);
    /// Queues a Fault; frames queued after it are dropped and transmit() fails.
    void report_fault(std::string detail);

    [[nodiscard]] bool started() const;
    /// The frames this link transmitted.
    [[nodiscard]] std::vector<Bytes> transmitted() const;
    /// Nothing queued, the handler not running.
    [[nodiscard]] bool idle() const;
    [[nodiscard]] int start_calls() const;
    [[nodiscard]] int stop_calls() const;

    // --- ExternalLink
    void start(EventHandler handler) override;
    void stop() noexcept override;
    eth_link::TransmitResult transmit(std::span<const std::uint8_t> frame) override;
    [[nodiscard]] bool echoes_own_transmissions() const noexcept override { return echo_own_.load(); }
    [[nodiscard]] std::string describe() const override;
    [[nodiscard]] std::string last_error() const override;

private:
    friend class FakeSegment;

    /// A queued event; the LinkEvent that points into it is made when it is delivered.
    struct Item
    {
        eth_link::LinkEventKind kind = eth_link::LinkEventKind::Frame;
        Bytes frame;
        std::uint64_t count = 0;
        eth_link::LinkState state = eth_link::LinkState::Unknown;
        std::string detail;
        bool checksum_incomplete = false;
    };

    void enqueue(Item item);
    void enqueue_frame(std::span<const std::uint8_t> frame);
    void run();

    FakeSegment& segment_;
    const std::string name_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Item> queue_;
    bool running_ = false;    // started and not stopped
    bool delivering_ = false; // the handler is running
    bool faulted_ = false;
    bool stop_requested_ = false;
    EventHandler handler_;
    std::thread thread_;
    std::vector<Bytes> transmitted_;
    std::optional<eth_link::LinkError> start_error_;
    eth_link::TransmitResult transmit_result_ = eth_link::TransmitResult::Ok;
    std::atomic<bool> echo_own_{false};
    bool transmit_throws_ = false;
    std::uint64_t overruns_at_stop_ = 0;
    int start_calls_ = 0;
    int stop_calls_ = 0;
    std::mutex stop_mutex_; // one stop() at a time
};

/// A frame: destination, source, EtherType (no VLAN tag), then `payload_bytes` bytes counting up
/// from `first_payload_byte`.
[[nodiscard]] Bytes make_frame(const std::string& destination, const std::string& source, std::uint16_t ether_type,
                               std::size_t payload_bytes = 46, std::uint8_t first_payload_byte = 0);
/// The same with one VLAN tag (TPID `tpid`, VLAN `vlan_id`) before the EtherType.
[[nodiscard]] Bytes make_tagged_frame(const std::string& destination, const std::string& source, std::uint16_t tpid,
                                      std::uint16_t vlan_id, std::uint16_t ether_type, std::size_t payload_bytes = 42);

} // namespace vehicle_sil::fake_eth
