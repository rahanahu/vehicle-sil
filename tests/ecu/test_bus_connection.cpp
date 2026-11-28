// SPDX-License-Identifier: MIT
//
// GatedCanPort and GatedEthPort: forward transmit_capabilities() and 2-arg send(frame, token)
// while connected, forward 1-arg send(frame) with token 0, and return SendResult::Failed without
// calling the inner port while disconnected.
#include "test_harness.hpp"

#include "vehicle_sil/ecu/detail/bus_connection.hpp"
#include "vehicle_sil/ecu/ecu.hpp"

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace {

namespace can = vehicle_sil::can;
namespace eth = vehicle_sil::eth;
using vehicle_sil::SimTime;
using vehicle_sil::detail::BusConnection;
using vehicle_sil::detail::GatedCanPort;
using vehicle_sil::detail::GatedEthPort;

class RecordingCanPort final : public can::Port
{
public:
    explicit RecordingCanPort(can::TransmitCapabilities cap) : cap_(cap) {}

    [[nodiscard]] can::TimeDomain time_domain() const noexcept override { return can::TimeDomain::Virtual; }
    [[nodiscard]] can::TransmitCapabilities transmit_capabilities() const noexcept override { return cap_; }
    std::size_t release_before(SimTime, can::FrameSink) override { return 0; }
    can::SendResult send(const can::TimedFrame& frame) override
    {
        one_arg_calls.push_back(frame.frame.id);
        return can::SendResult::Accepted;
    }
    can::SendResult send(const can::TimedFrame& frame, std::uint64_t token) override
    {
        two_arg_calls.emplace_back(frame.frame.id, token);
        return next_result;
    }

    can::SendResult next_result = can::SendResult::Accepted;
    std::vector<std::uint32_t> one_arg_calls;
    std::vector<std::pair<std::uint32_t, std::uint64_t>> two_arg_calls;

private:
    can::TransmitCapabilities cap_;
};

class RecordingEthPort final : public eth::Port
{
public:
    explicit RecordingEthPort(eth::TransmitCapabilities cap) : cap_(cap) {}

    [[nodiscard]] eth::TimeDomain time_domain() const noexcept override { return eth::TimeDomain::Virtual; }
    [[nodiscard]] eth::TransmitCapabilities transmit_capabilities() const noexcept override { return cap_; }
    std::size_t release_before(SimTime, eth::FrameSink) override { return 0; }
    eth::SendResult send(const eth::TimedFrame&) override
    {
        ++one_arg_calls;
        return eth::SendResult::Accepted;
    }
    eth::SendResult send(const eth::TimedFrame&, std::uint64_t token) override
    {
        two_arg_tokens.push_back(token);
        return next_result;
    }

    eth::SendResult next_result = eth::SendResult::Accepted;
    std::size_t one_arg_calls = 0;
    std::vector<std::uint64_t> two_arg_tokens;

private:
    eth::TransmitCapabilities cap_;
};

} // namespace

TEST_CASE(gated_can_port_forwards_capabilities_and_tokens_when_connected)
{
    BusConnection conn;
    RecordingCanPort noneInner(can::TransmitCapabilities::None);
    GatedCanPort noneGate(noneInner, conn);
    CHECK(noneGate.transmit_capabilities() == can::TransmitCapabilities::None);

    RecordingCanPort inner(can::TransmitCapabilities::Correlated);
    GatedCanPort gate(inner, conn);
    CHECK(gate.transmit_capabilities() == can::TransmitCapabilities::Correlated);

    const can::TimedFrame f{SimTime{}, can::Frame{0x123, can::IdFormat::Standard, 1, {0x42}}};
    CHECK(gate.send(f) == can::SendResult::Accepted);
    CHECK(gate.send(f, 0) == can::SendResult::Accepted);
    CHECK(gate.send(f, 99) == can::SendResult::Accepted);
    inner.next_result = can::SendResult::QueueFull;
    CHECK(gate.send(f, 100) == can::SendResult::QueueFull);
    CHECK(inner.one_arg_calls.empty());
    CHECK((inner.two_arg_calls == std::vector<std::pair<std::uint32_t, std::uint64_t>>{
                                      {0x123, 0}, {0x123, 0}, {0x123, 99}, {0x123, 100}}));

    conn.set(false);
    inner.next_result = can::SendResult::Accepted;
    CHECK(gate.send(f) == can::SendResult::Failed);
    CHECK(gate.send(f, 101) == can::SendResult::Failed);
    CHECK_EQ(inner.two_arg_calls.size(), 4u);
}

TEST_CASE(gated_eth_port_forwards_capabilities_and_tokens_when_connected)
{
    BusConnection conn;
    RecordingEthPort noneInner(eth::TransmitCapabilities::None);
    GatedEthPort noneGate(noneInner, conn);
    CHECK(noneGate.transmit_capabilities() == eth::TransmitCapabilities::None);

    RecordingEthPort inner(eth::TransmitCapabilities::Correlated);
    GatedEthPort gate(inner, conn);
    CHECK(gate.transmit_capabilities() == eth::TransmitCapabilities::Correlated);

    std::array<std::uint8_t, eth::kHeaderBytes> raw{};
    const eth::TimedFrame f{SimTime{}, *eth::Frame::from(raw)};
    CHECK(gate.send(f) == eth::SendResult::Accepted);
    CHECK(gate.send(f, 0) == eth::SendResult::Accepted);
    CHECK(gate.send(f, 77) == eth::SendResult::Accepted);
    inner.next_result = eth::SendResult::QueueFull;
    CHECK(gate.send(f, 78) == eth::SendResult::QueueFull);
    CHECK_EQ(inner.one_arg_calls, 0u);
    CHECK((inner.two_arg_tokens == std::vector<std::uint64_t>{0, 0, 77, 78}));

    conn.set(false);
    inner.next_result = eth::SendResult::Accepted;
    CHECK(gate.send(f) == eth::SendResult::Failed);
    CHECK(gate.send(f, 79) == eth::SendResult::Failed);
    CHECK_EQ(inner.two_arg_tokens.size(), 4u);
}

namespace {

class MinimalEcu final : public vehicle_sil::Ecu
{
public:
    MinimalEcu() : agent_("MinimalEcu: ", {}) {}

    void step(SimTime now, const std::function<void()>& body)
    {
        if (!started_)
        {
            (void)agent_.get();
            started_ = true;
        }
        agent_.begin(now);
        body();
        agent_.end(now);
    }

    [[nodiscard]] vehicle_sil::EcuDescription describe() const override { return {}; }
    void on_step(std::function<void(SimTime)>) override {}
    void every(vehicle_sil::SimDuration, std::function<void(SimTime)>) override {}
    void at(SimTime, std::function<void(SimTime)>) override {}

private:
    void require_not_started(const char*) const override {}
    [[nodiscard]] bool started() const noexcept override { return started_; }
    vehicle_sil::detail::EcuAgent& ecu_agent() noexcept override { return agent_; }
    vehicle_sil::j1939::Network& add_j1939(const std::string&, vehicle_sil::j1939::NetworkOptions) override
    {
        throw std::logic_error("unused");
    }
    vehicle_sil::can::RawChannel& add_can(const std::string&, vehicle_sil::can::RawChannelOptions) override
    {
        throw std::logic_error("unused");
    }
    vehicle_sil::lin::LinMaster& add_lin_master(const std::string&, vehicle_sil::lin::LinScheduleTable,
                                                vehicle_sil::lin::LinMasterOptions) override
    {
        throw std::logic_error("unused");
    }
    vehicle_sil::lin::LinSlave& add_lin_slave(const std::string&, vehicle_sil::lin::LinSlaveOptions) override
    {
        throw std::logic_error("unused");
    }
    vehicle_sil::eth::Channel& add_ethernet(const std::string&, vehicle_sil::eth::ChannelOptions) override
    {
        throw std::logic_error("unused");
    }
    void keep(std::shared_ptr<void>) override {}

    vehicle_sil::detail::EcuAgent agent_;
    bool started_ = false;
};

} // namespace

TEST_CASE(ecu_log_forwards_structured_events_with_step_timestamp)
{
    using namespace std::chrono_literals;
    namespace control = vehicle_sil::control;

    MinimalEcu ecu;
    ecu.step(SimTime{10ms}, [&] { ecu.log(control::EventLevel::Info, "booted"); });
    ecu.step(SimTime{50ms}, [&] {
        ecu.log(control::EventLevel::Warning, "temp high");
        ecu.log(control::EventLevel::Error, "fault active");
    });

    std::vector<control::Event> events;
    ecu.agent().drain_events(events);
    CHECK_EQ(events.size(), std::size_t{3});
    CHECK(events[0].time == SimTime{10ms});
    CHECK(events[0].level == control::EventLevel::Info);
    CHECK(events[0].text == "booted");
    CHECK(events[1].time == SimTime{50ms});
    CHECK(events[1].level == control::EventLevel::Warning);
    CHECK(events[1].text == "temp high");
    CHECK(events[2].time == SimTime{50ms});
    CHECK(events[2].level == control::EventLevel::Error);
    CHECK(events[2].text == "fault active");
}
