// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin/virtual_lin_bus.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace vehicle_sil::lin {
namespace {

constexpr const char* kWho = "vehicle_sil::lin::VirtualLinBus: ";

const char* role_name(LinRole role) noexcept
{
    return role == LinRole::Master ? "master" : "slave";
}

std::string quoted(std::string_view name)
{
    return "\"" + std::string(name) + "\"";
}

} // namespace

class VirtualLinBus::Delivery
{
public:
    Delivery(VirtualLinBus& bus, const Endpoint& sender, const char* call) : bus_(bus)
    {
        if (bus_.delivering_ != nullptr)
        {
            throw std::logic_error(std::string(kWho) + call + " of the endpoint " + quoted(sender.name()) +
                                   " while the bus delivers " + bus_.delivering_ +
                                   ": a listener or an observer must not send; send from the node's step instead");
        }
        bus_.delivering_ = call;
    }
    ~Delivery()
    {
        bus_.delivering_ = nullptr;
        bus_.responding_ = nullptr;
        bus_.responses_.clear();
    }
    Delivery(const Delivery&) = delete;
    Delivery& operator=(const Delivery&) = delete;

private:
    VirtualLinBus& bus_;
};

VirtualLinBus::~VirtualLinBus() = default;

void VirtualLinBus::Endpoint::require_role(LinRole role, const char* call) const
{
    if (role_ != role)
    {
        throw std::logic_error(std::string(kWho) + call + " is a function of a " + role_name(role) + ", and the endpoint " +
                               quoted(name_) + " is a " + role_name(role_) + "; attach a " + role_name(role) +
                               " endpoint for it");
    }
}

void VirtualLinBus::Endpoint::require_frame(LinId id, std::size_t data_length, const char* call) const
{
    if (!valid_lin_frame(id, data_length))
    {
        throw std::invalid_argument(std::string(kWho) + call + " of the master " + quoted(name_) + " with id " +
                                    std::to_string(id) + " and " + std::to_string(data_length) +
                                    " bytes: a LIN id is 0..63 and a frame has 1..8 bytes");
    }
}

void VirtualLinBus::Endpoint::send_header(SimTime now, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model)
{
    require_role(LinRole::Master, "send_header()");
    require_frame(id, data_length, "send_header()");
    bus_.deliver_header(*this, now, id, data_length, checksum_model);
}

void VirtualLinBus::Endpoint::send_frame(SimTime now, const LinFrame& frame)
{
    require_role(LinRole::Master, "send_frame()");
    require_frame(frame.id, frame.data_length, "send_frame()");
    bus_.deliver_frame(*this, now, frame);
}

void VirtualLinBus::Endpoint::send_go_to_sleep(SimTime now)
{
    require_role(LinRole::Master, "send_go_to_sleep()");
    bus_.deliver_go_to_sleep(*this, now);
}

void VirtualLinBus::Endpoint::respond(SimTime header_time, const LinFrame& frame)
{
    require_role(LinRole::Slave, "respond()");
    bus_.accept_response(*this, header_time, frame);
}

void VirtualLinBus::Endpoint::send_wakeup(SimTime now)
{
    bus_.deliver_wakeup(*this, now);
}

VirtualLinBus::Endpoint& VirtualLinBus::attach(LinRole role, std::string name)
{
    if (delivering_ != nullptr)
    {
        throw std::logic_error(std::string(kWho) + "attach(" + quoted(name) + ") while the bus delivers " + delivering_ +
                               "; attach the endpoints before the run");
    }
    if (role == LinRole::Master && master_ != nullptr)
    {
        throw std::logic_error(std::string(kWho) + "attach(master, " + quoted(name) + "): the bus has a master, " +
                               quoted(master_->name()) + "; a LIN bus has one master, make " + quoted(name) +
                               " a slave or put it on another bus");
    }
    endpoints_.reserve(endpoints_.size() + 1);
    endpoints_.push_back(std::unique_ptr<Endpoint>(new Endpoint(*this, role, std::move(name))));
    Endpoint& endpoint = *endpoints_.back();
    if (role == LinRole::Master)
    {
        master_ = &endpoint;
    }
    return endpoint;
}

void VirtualLinBus::add_observer(std::function<void(const LinBusEvent&)> observer)
{
    if (!observer)
    {
        throw std::invalid_argument(std::string(kWho) + "add_observer(): empty function");
    }
    if (delivering_ != nullptr)
    {
        throw std::logic_error(std::string(kWho) + "add_observer() while the bus delivers " + delivering_ +
                               "; add the observers before the run");
    }
    observers_.push_back(std::move(observer));
}

void VirtualLinBus::notify(LinBusEvent::Kind kind, SimTime time, std::string_view sender, const LinFrame& frame)
{
    const LinBusEvent event{kind, time, sender, frame};
    for (const auto& observer : observers_)
    {
        observer(event);
    }
}

std::vector<VirtualLinBus::Response> VirtualLinBus::collect_responses(SimTime now, LinId id)
{
    header_time_ = now;
    header_id_ = id;
    responses_.clear();
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint->role_ != LinRole::Slave || endpoint->listener_ == nullptr)
        {
            continue;
        }
        responding_ = endpoint.get();
        endpoint->listener_->on_header(now, id);
        responding_ = nullptr;
    }
    return std::exchange(responses_, {});
}

void VirtualLinBus::accept_response(const Endpoint& slave, SimTime header_time, const LinFrame& frame)
{
    if (responding_ != &slave)
    {
        throw std::logic_error(std::string(kWho) + "respond() of the slave " + quoted(slave.name()) +
                               " outside its on_header(): a slave answers a header only from inside on_header()");
    }
    if (header_time != header_time_ || frame.id != header_id_)
    {
        throw std::invalid_argument(std::string(kWho) + "respond() of the slave " + quoted(slave.name()) + " with id " +
                                    std::to_string(frame.id) + " at " +
                                    std::to_string(header_time.time_since_epoch().count()) + " ns to the header of id " +
                                    std::to_string(header_id_) + " at " +
                                    std::to_string(header_time_.time_since_epoch().count()) +
                                    " ns: answer with the header's id and time");
    }
    if (!valid_lin_frame(frame.id, frame.data_length))
    {
        throw std::invalid_argument(std::string(kWho) + "respond() of the slave " + quoted(slave.name()) + " to id " +
                                    std::to_string(frame.id) + " with " + std::to_string(frame.data_length) +
                                    " bytes: a response has 1..8 bytes");
    }
    for (const auto& response : responses_)
    {
        if (response.slave == &slave)
        {
            throw std::logic_error(std::string(kWho) + "respond() of the slave " + quoted(slave.name()) +
                                   " a second time to the header of id " + std::to_string(header_id_) +
                                   ": a slave answers a header once; call respond() once per on_header()");
        }
    }
    responses_.push_back(Response{&slave, frame});
}

void VirtualLinBus::deliver_header(const Endpoint& master, SimTime now, LinId id, std::uint8_t data_length,
                                   LinChecksumModel checksum_model)
{
    const Delivery delivery(*this, master, "send_header()");
    const auto responses = collect_responses(now, id);
    LinPortListener* listener = master.listener_;
    if (responses.empty())
    {
        LinFrame none;
        none.id = id;
        none.checksum_model = checksum_model;
        if (listener != nullptr)
        {
            listener->on_response(now, none, LinResultStatus::NoResponse);
        }
        notify(LinBusEvent::Kind::NoResponse, now, master.name(), none);
        return;
    }
    for (const auto& response : responses)
    {
        const bool matches =
            response.frame.data_length == data_length && response.frame.checksum_model == checksum_model;
        if (listener != nullptr)
        {
            listener->on_response(now, response.frame, matches ? LinResultStatus::Ok : LinResultStatus::Error);
        }
    }
    if (responses.size() == 1)
    {
        notify(LinBusEvent::Kind::Frame, now, responses.front().slave->name(), responses.front().frame);
    }
    else
    {
        LinFrame collided;
        collided.id = id;
        notify(LinBusEvent::Kind::Collision, now, master.name(), collided);
    }
}

void VirtualLinBus::deliver_frame(const Endpoint& master, SimTime now, const LinFrame& frame)
{
    const Delivery delivery(*this, master, "send_frame()");
    const auto responses = collect_responses(now, frame.id);
    LinPortListener* listener = master.listener_;
    if (listener != nullptr)
    {
        listener->on_response(now, frame, LinResultStatus::Ok);
    }
    if (responses.empty())
    {
        for (const auto& endpoint : endpoints_)
        {
            if (endpoint->role_ == LinRole::Slave && endpoint->listener_ != nullptr)
            {
                endpoint->listener_->on_response(now, frame, LinResultStatus::Ok);
            }
        }
        notify(LinBusEvent::Kind::Frame, now, master.name(), frame);
        return;
    }
    // A slave answered the master's own frame: the responses collide. The master gets each of
    // them after its own frame; the slaves get nothing.
    for (const auto& response : responses)
    {
        const bool matches =
            response.frame.data_length == frame.data_length && response.frame.checksum_model == frame.checksum_model;
        if (listener != nullptr)
        {
            listener->on_response(now, response.frame, matches ? LinResultStatus::Ok : LinResultStatus::Error);
        }
    }
    LinFrame collided;
    collided.id = frame.id;
    notify(LinBusEvent::Kind::Collision, now, master.name(), collided);
}

void VirtualLinBus::deliver_go_to_sleep(const Endpoint& master, SimTime now)
{
    const Delivery delivery(*this, master, "send_go_to_sleep()");
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint->role_ == LinRole::Slave && endpoint->listener_ != nullptr)
        {
            endpoint->listener_->on_go_to_sleep(now);
        }
    }
    notify(LinBusEvent::Kind::GoToSleep, now, master.name(), LinFrame{});
}

void VirtualLinBus::deliver_wakeup(const Endpoint& sender, SimTime now)
{
    const Delivery delivery(*this, sender, "send_wakeup()");
    for (const auto& endpoint : endpoints_)
    {
        if (endpoint.get() != &sender && endpoint->listener_ != nullptr)
        {
            endpoint->listener_->on_wakeup(now);
        }
    }
    notify(LinBusEvent::Kind::Wakeup, now, sender.name(), LinFrame{});
}

} // namespace vehicle_sil::lin
