// SPDX-License-Identifier: MIT
#include "fake_link.hpp"

#include "vehicle_sil/eth_link/mac_address.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace vehicle_sil::fake_eth {
namespace {

using eth_link::LinkEvent;
using eth_link::LinkEventKind;
using eth_link::TransmitResult;

void append_mac(Bytes& frame, const std::string& text)
{
    const auto mac = eth_link::parse_mac_address(text);
    if (!mac)
    {
        throw std::invalid_argument("make_frame: not a MAC address: " + text);
    }
    frame.insert(frame.end(), mac->bytes.begin(), mac->bytes.end());
}

void append_be16(Bytes& frame, std::uint16_t value)
{
    frame.push_back(static_cast<std::uint8_t>(value >> 8));
    frame.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_payload(Bytes& frame, std::size_t bytes, std::uint8_t first)
{
    for (std::size_t i = 0; i < bytes; ++i)
    {
        frame.push_back(static_cast<std::uint8_t>(first + i));
    }
}

} // namespace

// --- FakeSegment

void FakeSegment::send(std::span<const std::uint8_t> frame)
{
    const std::lock_guard lock(mutex_);
    for (auto* link : links_)
    {
        link->enqueue_frame(frame);
    }
}

std::vector<Bytes> FakeSegment::transmitted() const
{
    const std::lock_guard lock(mutex_);
    return transmitted_;
}

bool FakeSegment::idle() const
{
    const std::lock_guard lock(mutex_);
    return std::all_of(links_.begin(), links_.end(), [](const FakeLink* link) { return link->idle(); });
}

void FakeSegment::attach(FakeLink& link)
{
    const std::lock_guard lock(mutex_);
    links_.push_back(&link);
}

void FakeSegment::detach(FakeLink& link)
{
    const std::lock_guard lock(mutex_);
    links_.erase(std::remove(links_.begin(), links_.end(), &link), links_.end());
}

void FakeSegment::transmit_from(const FakeLink& sender, std::span<const std::uint8_t> frame, bool echo_own)
{
    const std::lock_guard lock(mutex_);
    transmitted_.emplace_back(frame.begin(), frame.end());
    for (auto* link : links_)
    {
        if (link != &sender || echo_own)
        {
            link->enqueue_frame(frame);
        }
    }
}

// --- FakeLink

FakeLink::FakeLink(FakeSegment& segment, std::string name) :
  segment_(segment),
  name_(std::move(name))
{
    segment_.attach(*this);
}

FakeLink::~FakeLink()
{
    stop();
    segment_.detach(*this);
}

void FakeLink::fail_start(eth_link::LinkErrorKind kind, std::string message)
{
    const std::lock_guard lock(mutex_);
    start_error_.emplace(kind, message);
}

void FakeLink::set_transmit_result(TransmitResult result)
{
    const std::lock_guard lock(mutex_);
    transmit_result_ = result;
}

void FakeLink::set_echo_own(bool echo)
{
    echo_own_.store(echo);
}

void FakeLink::set_transmit_throws(bool throws)
{
    const std::lock_guard lock(mutex_);
    transmit_throws_ = throws;
}

void FakeLink::receive(std::span<const std::uint8_t> frame)
{
    enqueue_frame(frame);
}

void FakeLink::receive_checksum_incomplete(std::span<const std::uint8_t> frame)
{
    enqueue(Item{LinkEventKind::Frame, Bytes(frame.begin(), frame.end()), 0, {}, {}, true});
}

void FakeLink::set_overruns_at_stop(std::uint64_t count)
{
    const std::lock_guard lock(mutex_);
    overruns_at_stop_ = count;
}

void FakeLink::report_overrun(std::uint64_t count)
{
    enqueue(Item{LinkEventKind::Overrun, {}, count, {}, {}});
}

void FakeLink::report_link_state(eth_link::LinkState state)
{
    enqueue(Item{LinkEventKind::LinkState, {}, 0, state, {}});
}

void FakeLink::report_warning(std::string detail)
{
    enqueue(Item{LinkEventKind::Warning, {}, 0, {}, std::move(detail)});
}

void FakeLink::report_fault(std::string detail)
{
    enqueue(Item{LinkEventKind::Fault, {}, 0, {}, std::move(detail)});
}

bool FakeLink::started() const
{
    const std::lock_guard lock(mutex_);
    return running_;
}

std::vector<Bytes> FakeLink::transmitted() const
{
    const std::lock_guard lock(mutex_);
    return transmitted_;
}

bool FakeLink::idle() const
{
    const std::lock_guard lock(mutex_);
    return queue_.empty() && !delivering_;
}

int FakeLink::start_calls() const
{
    const std::lock_guard lock(mutex_);
    return start_calls_;
}

int FakeLink::stop_calls() const
{
    const std::lock_guard lock(mutex_);
    return stop_calls_;
}

void FakeLink::enqueue(Item item)
{
    {
        const std::lock_guard lock(mutex_);
        if (!running_ || (faulted_ && item.kind == LinkEventKind::Frame))
        {
            return;
        }
        queue_.push_back(std::move(item));
    }
    cv_.notify_all();
}

void FakeLink::enqueue_frame(std::span<const std::uint8_t> frame)
{
    enqueue(Item{LinkEventKind::Frame, Bytes(frame.begin(), frame.end()), 0, {}, {}});
}

void FakeLink::start(EventHandler handler)
{
    const std::lock_guard stopLock(stop_mutex_);
    const std::lock_guard lock(mutex_);
    ++start_calls_;
    if (start_error_)
    {
        throw *start_error_;
    }
    if (running_)
    {
        throw std::logic_error("FakeLink: started twice");
    }
    handler_ = std::move(handler);
    running_ = true;
    faulted_ = false;
    stop_requested_ = false;
    thread_ = std::thread([this] { run(); });
}

void FakeLink::run()
{
    std::unique_lock lock(mutex_);
    for (;;)
    {
        cv_.wait(lock, [this] { return stop_requested_ || !queue_.empty(); });
        if (stop_requested_)
        {
            return;
        }
        Item item = std::move(queue_.front());
        queue_.pop_front();
        if (faulted_ && item.kind == LinkEventKind::Frame)
        {
            continue;
        }
        if (item.kind == LinkEventKind::Fault)
        {
            faulted_ = true;
        }
        delivering_ = true;
        lock.unlock();
        LinkEvent event;
        event.kind = item.kind;
        event.frame = item.frame;
        event.count = item.count;
        event.state = item.state;
        event.detail = item.detail;
        event.checksum_incomplete = item.checksum_incomplete;
        handler_(event); // handler_ changes only while the thread is not running
        lock.lock();
        delivering_ = false;
    }
}

void FakeLink::stop() noexcept
{
    const std::lock_guard stopLock(stop_mutex_);
    std::uint64_t overruns = 0;
    {
        const std::lock_guard lock(mutex_);
        ++stop_calls_;
        if (!running_)
        {
            return;
        }
        running_ = false;
        stop_requested_ = true;
        queue_.clear();
        overruns = overruns_at_stop_;
    }
    cv_.notify_all();
    thread_.join();
    if (overruns > 0)
    {
        LinkEvent event;
        event.kind = LinkEventKind::Overrun;
        event.count = overruns;
        handler_(event);
    }
    const std::lock_guard lock(mutex_);
    handler_ = nullptr;
}

TransmitResult FakeLink::transmit(std::span<const std::uint8_t> frame)
{
    bool echo = false;
    {
        const std::lock_guard lock(mutex_);
        if (!running_ || faulted_)
        {
            return TransmitResult::Failed;
        }
        if (transmit_throws_)
        {
            throw std::runtime_error("fake link '" + name_ + "' threw in transmit()");
        }
        if (transmit_result_ != TransmitResult::Ok)
        {
            return transmit_result_;
        }
        transmitted_.emplace_back(frame.begin(), frame.end());
        echo = echo_own_.load();
    }
    segment_.transmit_from(*this, frame, echo);
    return TransmitResult::Ok;
}

std::string FakeLink::describe() const
{
    return "fake '" + name_ + "' (virtual)";
}

std::string FakeLink::last_error() const
{
    return "fake link '" + name_ + "' refused the frame";
}

// --- frames

Bytes make_frame(const std::string& destination, const std::string& source, std::uint16_t ether_type,
                 std::size_t payload_bytes, std::uint8_t first_payload_byte)
{
    Bytes frame;
    append_mac(frame, destination);
    append_mac(frame, source);
    append_be16(frame, ether_type);
    append_payload(frame, payload_bytes, first_payload_byte);
    return frame;
}

Bytes make_tagged_frame(const std::string& destination, const std::string& source, std::uint16_t tpid,
                        std::uint16_t vlan_id, std::uint16_t ether_type, std::size_t payload_bytes)
{
    Bytes frame;
    append_mac(frame, destination);
    append_mac(frame, source);
    append_be16(frame, tpid);
    append_be16(frame, vlan_id);
    append_be16(frame, ether_type);
    append_payload(frame, payload_bytes, 0);
    return frame;
}

} // namespace vehicle_sil::fake_eth
