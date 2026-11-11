// SPDX-License-Identifier: MIT
//
// lin::VirtualLinBus alone, with fake nodes: who gets what and in which order, no response,
// length and checksum mismatches, collisions, go-to-sleep, wakeup, observers and misuse.
#include "test_harness.hpp"

#include "vehicle_sil/lin/virtual_lin_bus.hpp"

#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::lin;
using vehicle_sil::unit::Failure;
using vehicle_sil::unit::throws;
using vehicle_sil::unit::throws_with;

SimTime at(std::chrono::milliseconds ms)
{
    return SimTime{} + ms;
}

long long ms_of(SimTime t)
{
    return static_cast<long long>(t.time_since_epoch() / 1ms);
}

std::string hex(const LinFrame& f)
{
    std::string s;
    char buf[4];
    for (std::size_t i = 0; i < f.data_length && i < f.data.size(); ++i)
    {
        std::snprintf(buf, sizeof(buf), "%02X", f.data[i]);
        s += buf;
    }
    return s;
}

const char* status_name(LinResultStatus s)
{
    switch (s)
    {
    case LinResultStatus::Ok:
        return "ok";
    case LinResultStatus::NoResponse:
        return "no_response";
    case LinResultStatus::Error:
        return "error";
    }
    return "?";
}

const char* kind_name(LinBusEvent::Kind k)
{
    switch (k)
    {
    case LinBusEvent::Kind::Frame:
        return "frame";
    case LinBusEvent::Kind::NoResponse:
        return "no_response";
    case LinBusEvent::Kind::Collision:
        return "collision";
    case LinBusEvent::Kind::GoToSleep:
        return "go_to_sleep";
    case LinBusEvent::Kind::Wakeup:
        return "wakeup";
    }
    return "?";
}

LinFrame frame(LinId id, std::vector<std::uint8_t> data, LinChecksumModel checksum = LinChecksumModel::Enhanced)
{
    LinFrame f;
    f.id = id;
    f.checksum_model = checksum;
    f.data_length = static_cast<std::uint8_t>(data.size());
    for (std::size_t i = 0; i < data.size(); ++i)
    {
        f.data[i] = data[i];
    }
    return f;
}

void require_lines(const std::vector<std::string>& actual, const std::vector<std::string>& expected)
{
    if (actual == expected)
    {
        return;
    }
    std::string message = "lines differ.\n  actual:";
    for (const auto& line : actual)
    {
        message += "\n    " + line;
    }
    message += "\n  expected:";
    for (const auto& line : expected)
    {
        message += "\n    " + line;
    }
    throw Failure(message);
}

/// A node that writes every event of its port to a shared log and answers headers with
/// `answer` (if set).
class FakeNode final : public LinPortListener
{
public:
    FakeNode(std::string name, std::vector<std::string>& log) : name_(std::move(name)), log_(log) {}

    LinPort* port = nullptr;
    /// Called in on_header(); may respond through `port`.
    std::function<void(SimTime, LinId)> answer;
    /// Called in the other events (to try sending from a listener).
    std::function<void()> inside;

    void on_header(SimTime time, LinId id) override
    {
        log_.push_back(name_ + " header t=" + std::to_string(ms_of(time)) + " id=" + std::to_string(id));
        if (answer)
        {
            answer(time, id);
        }
    }
    void on_response(SimTime header_time, const LinFrame& f, LinResultStatus status) override
    {
        log_.push_back(name_ + " response t=" + std::to_string(ms_of(header_time)) + " id=" + std::to_string(f.id) + " " +
                       status_name(status) + (status == LinResultStatus::Ok ? " data=" + hex(f) : std::string{}));
        if (inside)
        {
            inside();
        }
    }
    void on_go_to_sleep(SimTime time) override
    {
        log_.push_back(name_ + " go_to_sleep t=" + std::to_string(ms_of(time)));
    }
    void on_wakeup(SimTime time) override
    {
        log_.push_back(name_ + " wakeup t=" + std::to_string(ms_of(time)));
    }

private:
    std::string name_;
    std::vector<std::string>& log_;
};

/// A bus with a master "M" and slaves "A" and "B" (attached in that order), and an observer that
/// writes to the same log.
struct Bench
{
    std::vector<std::string> log;
    VirtualLinBus bus;
    FakeNode master{"M", log};
    FakeNode a{"A", log};
    FakeNode b{"B", log};
    VirtualLinBus::Endpoint* m_port = nullptr;
    VirtualLinBus::Endpoint* a_port = nullptr;
    VirtualLinBus::Endpoint* b_port = nullptr;

    Bench()
    {
        m_port = &bus.attach(LinRole::Master, "M");
        a_port = &bus.attach(LinRole::Slave, "A");
        b_port = &bus.attach(LinRole::Slave, "B");
        for (auto [node, port] : {std::pair{&master, m_port}, std::pair{&a, a_port}, std::pair{&b, b_port}})
        {
            node->port = port;
            port->set_listener(node);
        }
        bus.add_observer([this](const LinBusEvent& e) {
            log.push_back(std::string("observer ") + kind_name(e.kind) + " t=" + std::to_string(ms_of(e.time)) + " from=" +
                          std::string(e.sender) + " id=" + std::to_string(e.frame.id) + " data=" + hex(e.frame));
        });
    }
    ~Bench()
    {
        for (auto* port : {m_port, a_port, b_port})
        {
            port->set_listener(nullptr);
        }
    }
    Bench(const Bench&) = delete;
    Bench& operator=(const Bench&) = delete;
};

TEST_CASE(lin_bus_header_goes_to_every_slave_in_order_and_the_response_to_the_master)
{
    Bench t;
    t.b.answer = [&](SimTime time, LinId id) { t.b_port->respond(time, frame(id, {0x12, 0x34})); };
    t.m_port->send_header(at(10ms), 0x20, 2, LinChecksumModel::Enhanced);
    require_lines(t.log, {"A header t=10 id=32", "B header t=10 id=32", "M response t=10 id=32 ok data=1234",
                          "observer frame t=10 from=B id=32 data=1234"});
    CHECK(t.m_port->name() == "M");
    CHECK(t.b_port->role() == LinRole::Slave);
}

TEST_CASE(lin_bus_reports_no_response_inside_the_send)
{
    Bench t;
    t.m_port->send_header(at(20ms), 0x21, 1, LinChecksumModel::Classic);
    // Synchronous: the master has its result when send_header() returns.
    require_lines(t.log, {"A header t=20 id=33", "B header t=20 id=33", "M response t=20 id=33 no_response",
                          "observer no_response t=20 from=M id=33 data="});
}

TEST_CASE(lin_bus_makes_a_length_or_checksum_mismatch_an_error)
{
    Bench t;
    t.a.answer = [&](SimTime time, LinId id) {
        if (id == 0x20)
        {
            t.a_port->respond(time, frame(id, {0x01})); // 1 byte, the header says 2
        }
        else
        {
            t.a_port->respond(time, frame(id, {0x01, 0x02}, LinChecksumModel::Classic)); // the header says enhanced
        }
    };
    t.m_port->send_header(at(0ms), 0x20, 2, LinChecksumModel::Enhanced);
    t.m_port->send_header(at(10ms), 0x22, 2, LinChecksumModel::Enhanced);
    require_lines(t.log, {"A header t=0 id=32", "B header t=0 id=32", "M response t=0 id=32 error",
                          "observer frame t=0 from=A id=32 data=01", "A header t=10 id=34", "B header t=10 id=34",
                          "M response t=10 id=34 error", "observer frame t=10 from=A id=34 data=0102"});
}

TEST_CASE(lin_bus_gives_the_master_every_response_of_a_collision)
{
    Bench t;
    t.a.answer = [&](SimTime time, LinId id) { t.a_port->respond(time, frame(id, {0xA0})); };
    t.b.answer = [&](SimTime time, LinId id) { t.b_port->respond(time, frame(id, {0xB0})); };
    t.m_port->send_header(at(30ms), 0x30, 1, LinChecksumModel::Enhanced);
    require_lines(t.log, {"A header t=30 id=48", "B header t=30 id=48", "M response t=30 id=48 ok data=A0",
                          "M response t=30 id=48 ok data=B0", "observer collision t=30 from=M id=48 data="});
}

TEST_CASE(lin_bus_delivers_a_master_frame_to_the_master_and_every_slave)
{
    Bench t;
    t.m_port->send_frame(at(40ms), frame(0x10, {0x28, 0x00}));
    require_lines(t.log, {"A header t=40 id=16", "B header t=40 id=16", "M response t=40 id=16 ok data=2800",
                          "A response t=40 id=16 ok data=2800", "B response t=40 id=16 ok data=2800",
                          "observer frame t=40 from=M id=16 data=2800"});
}

TEST_CASE(lin_bus_makes_a_slave_answering_a_master_frame_a_collision)
{
    Bench t;
    t.b.answer = [&](SimTime time, LinId id) { t.b_port->respond(time, frame(id, {0x22})); };
    t.m_port->send_frame(at(50ms), frame(0x05, {0x11}));
    // The master gets its own frame and the response (LinMaster makes it an error); the slaves
    // get nothing.
    require_lines(t.log, {"A header t=50 id=5", "B header t=50 id=5", "M response t=50 id=5 ok data=11",
                          "M response t=50 id=5 ok data=22", "observer collision t=50 from=M id=5 data="});
}

TEST_CASE(lin_bus_sends_go_to_sleep_to_the_slaves_and_wakeup_to_the_others)
{
    Bench t;
    t.m_port->send_go_to_sleep(at(100ms));
    t.a_port->send_wakeup(at(202ms));
    t.m_port->send_wakeup(at(300ms));
    t.m_port->resume(); // nothing on the bus
    t.a_port->open();
    require_lines(t.log, {"A go_to_sleep t=100", "B go_to_sleep t=100", "observer go_to_sleep t=100 from=M id=0 data=",
                          "M wakeup t=202", "B wakeup t=202", "observer wakeup t=202 from=A id=0 data=",
                          "A wakeup t=300", "B wakeup t=300", "observer wakeup t=300 from=M id=0 data="});
}

TEST_CASE(lin_bus_skips_endpoints_without_a_listener)
{
    Bench t;
    t.a_port->set_listener(nullptr);
    t.m_port->send_frame(at(0ms), frame(0x10, {0x01}));
    t.m_port->set_listener(nullptr);
    t.m_port->send_header(at(10ms), 0x20, 1, LinChecksumModel::Enhanced);
    require_lines(t.log, {"B header t=0 id=16", "M response t=0 id=16 ok data=01", "B response t=0 id=16 ok data=01",
                          "observer frame t=0 from=M id=16 data=01", "B header t=10 id=32",
                          "observer no_response t=10 from=M id=32 data="});
}

TEST_CASE(lin_bus_refuses_misuse)
{
    Bench t;
    // One master per bus; the message names both.
    CHECK(throws_with<std::logic_error>([&] { t.bus.attach(LinRole::Master, "M2"); }, "\"M2\""));
    CHECK(throws_with<std::logic_error>([&] { t.bus.attach(LinRole::Master, "M2"); }, "\"M\""));
    // A function of the other role.
    CHECK(throws_with<std::logic_error>([&] { t.a_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); },
                                        "\"A\""));
    CHECK(throws<std::logic_error>([&] { t.a_port->send_frame(at(0ms), frame(0x10, {1})); }));
    CHECK(throws<std::logic_error>([&] { t.a_port->send_go_to_sleep(at(0ms)); }));
    CHECK(throws_with<std::logic_error>([&] { t.m_port->respond(at(0ms), frame(0x10, {1})); }, "\"M\""));
    // respond() only from the slave's own on_header().
    CHECK(throws_with<std::logic_error>([&] { t.a_port->respond(at(0ms), frame(0x10, {1})); }, "on_header"));
    t.a.answer = [&](SimTime time, LinId id) { t.b_port->respond(time, frame(id, {1})); }; // B in A's on_header
    CHECK(throws_with<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); },
                                        "\"B\""));
    // A response with another id or an invalid length.
    t.a.answer = [&](SimTime time, LinId) { t.a_port->respond(time, frame(0x11, {1})); };
    CHECK(throws<std::invalid_argument>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    t.a.answer = [&](SimTime time, LinId) { t.a_port->respond(time - 1ms, frame(0x10, {1})); };
    CHECK(throws<std::invalid_argument>([&] { t.m_port->send_header(at(5ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    t.a.answer = [&](SimTime time, LinId id) { t.a_port->respond(time, frame(id, {})); };
    CHECK(throws<std::invalid_argument>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    // A second respond() in one on_header(); the message names the slave.
    t.a.answer = [&](SimTime time, LinId id) {
        t.a_port->respond(time, frame(id, {1}));
        t.a_port->respond(time, frame(id, {2}));
    };
    CHECK(throws_with<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); },
                                        "\"A\" a second time"));
    // The master's headers and frames: an id above 63, a length outside 1..8. Nothing is delivered.
    t.a.answer = {};
    t.log.clear();
    CHECK(throws_with<std::invalid_argument>([&] { t.m_port->send_header(at(0ms), 64, 1, LinChecksumModel::Enhanced); },
                                             "id 64"));
    CHECK(throws_with<std::invalid_argument>([&] { t.m_port->send_header(at(0ms), 0x10, 0, LinChecksumModel::Enhanced); },
                                             "0 bytes"));
    CHECK(throws_with<std::invalid_argument>([&] { t.m_port->send_header(at(0ms), 0x10, 9, LinChecksumModel::Enhanced); },
                                             "9 bytes"));
    CHECK(throws_with<std::invalid_argument>([&] { t.m_port->send_frame(at(0ms), frame(64, {1})); }, "\"M\""));
    CHECK(throws<std::invalid_argument>([&] { t.m_port->send_frame(at(0ms), frame(0x10, {})); }));
    LinFrame tooLong = frame(0x10, {1});
    tooLong.data_length = 9;
    CHECK(throws<std::invalid_argument>([&] { t.m_port->send_frame(at(0ms), tooLong); }));
    require_lines(t.log, {});
    // No sending, attaching or observing from inside a delivery.
    t.a.answer = {};
    t.master.inside = [&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); };
    CHECK(throws_with<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); },
                                        "must not send"));
    t.master.inside = [&] { t.a_port->send_wakeup(at(0ms)); };
    CHECK(throws<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    t.master.inside = [&] { t.bus.attach(LinRole::Slave, "C"); };
    CHECK(throws<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    t.master.inside = [&] { t.bus.add_observer([](const LinBusEvent&) {}); };
    CHECK(throws<std::logic_error>([&] { t.m_port->send_header(at(0ms), 0x10, 1, LinChecksumModel::Enhanced); }));
    CHECK(throws<std::invalid_argument>([&] { t.bus.add_observer({}); }));
    // After the failures the bus works again.
    t.master.inside = {};
    t.log.clear();
    t.m_port->send_header(at(60ms), 0x10, 1, LinChecksumModel::Enhanced);
    require_lines(t.log, {"A header t=60 id=16", "B header t=60 id=16", "M response t=60 id=16 no_response",
                          "observer no_response t=60 from=M id=16 data="});
    // A second slave is fine.
    auto& c = t.bus.attach(LinRole::Slave, "C");
    CHECK(c.role() == LinRole::Slave);
}

} // namespace
