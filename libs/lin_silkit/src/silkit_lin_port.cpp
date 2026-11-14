// SPDX-License-Identifier: MIT
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/experimental/services/lin/LinControllerExtensions.hpp>

#include <exception>
#include <stdexcept>
#include <string>

namespace vehicle_sil::lin {
namespace {

namespace silkit_lin = SilKit::Services::Lin;
namespace experimental_lin = SilKit::Experimental::Services::Lin;

// Only used by a detailed (network simulator) LIN simulation; the trivial simulation ignores it.
constexpr silkit_lin::LinBaudRate kBaudRate = 19200;

/// Removes a SIL Kit handler in a destructor. The participant may already be going away, in
/// which case nothing else can call the handler.
template <typename Remove>
void remove_handler(const std::optional<SilKit::Util::HandlerId>& id, Remove&& remove) noexcept
{
    if (!id)
    {
        return;
    }
    try
    {
        remove(*id);
    }
    catch (const std::exception&)
    {
    }
}

silkit_lin::LinChecksumModel to_silkit(LinChecksumModel model) noexcept
{
    return model == LinChecksumModel::Classic ? silkit_lin::LinChecksumModel::Classic
                                              : silkit_lin::LinChecksumModel::Enhanced;
}

silkit_lin::LinFrame to_silkit(const LinFrame& frame) noexcept
{
    silkit_lin::LinFrame out;
    out.id = frame.id;
    out.checksumModel = to_silkit(frame.checksum_model);
    out.dataLength = frame.data_length;
    out.data = frame.data;
    return out;
}

/// SIL Kit's Unknown checksum model never comes from a vehicle-sil node; it becomes Enhanced.
LinFrame from_silkit(const silkit_lin::LinFrame& frame) noexcept
{
    LinFrame out;
    out.id = frame.id;
    out.checksum_model =
        frame.checksumModel == silkit_lin::LinChecksumModel::Classic ? LinChecksumModel::Classic : LinChecksumModel::Enhanced;
    out.data_length = frame.dataLength;
    out.data = frame.data;
    return out;
}

LinResultStatus to_result_status(silkit_lin::LinFrameStatus status) noexcept
{
    switch (status)
    {
    case silkit_lin::LinFrameStatus::LIN_RX_OK:
    case silkit_lin::LinFrameStatus::LIN_TX_OK:
        return LinResultStatus::Ok;
    case silkit_lin::LinFrameStatus::LIN_RX_NO_RESPONSE:
        return LinResultStatus::NoResponse;
    default:
        return LinResultStatus::Error;
    }
}

bool is_go_to_sleep_frame(const silkit_lin::LinFrame& frame)
{
    const auto command = silkit_lin::GoToSleepFrame();
    return frame.id == command.id && frame.data == command.data;
}

const char* role_name(LinRole role) noexcept
{
    return role == LinRole::Master ? "master" : "slave";
}

} // namespace

SilKitLinPort::SilKitLinPort(silkit_lin::ILinController& controller, LinRole role) : controller_(controller), role_(role)
{
    try
    {
        if (role_ == LinRole::Slave)
        {
            // The header handler is added in open(), after InitDynamic(): SIL Kit's C API calls
            // adding it before that a state error.
            sleep_handler_ = controller_.AddGoToSleepHandler(
                [this](silkit_lin::ILinController*, const silkit_lin::LinGoToSleepEvent& event) {
                    if (auto* listener = listener_.load(std::memory_order_acquire))
                    {
                        listener->on_go_to_sleep(from_silkit_time(event.timestamp));
                    }
                });
        }
        status_handler_ = controller_.AddFrameStatusHandler(
            [this](silkit_lin::ILinController*, const silkit_lin::LinFrameStatusEvent& event) { on_frame_status(event); });
        wakeup_handler_ =
            controller_.AddWakeupHandler([this](silkit_lin::ILinController*, const silkit_lin::LinWakeupEvent& event) {
                // TX: the pulse this node sent itself.
                if (event.direction != SilKit::Services::TransmitDirection::RX)
                {
                    return;
                }
                if (auto* listener = listener_.load(std::memory_order_acquire))
                {
                    listener->on_wakeup(from_silkit_time(event.timestamp));
                }
            });
    }
    catch (...)
    {
        remove_handlers(); // the ones added so far
        throw;
    }
}

SilKitLinPort::~SilKitLinPort()
{
    remove_handlers();
}

void SilKitLinPort::remove_handlers() noexcept
{
    remove_handler(header_handler_, [this](auto id) { experimental_lin::RemoveFrameHeaderHandler(&controller_, id); });
    remove_handler(status_handler_, [this](auto id) { controller_.RemoveFrameStatusHandler(id); });
    remove_handler(sleep_handler_, [this](auto id) { controller_.RemoveGoToSleepHandler(id); });
    remove_handler(wakeup_handler_, [this](auto id) { controller_.RemoveWakeupHandler(id); });
}

void SilKitLinPort::set_listener(LinPortListener* listener)
{
    listener_.store(listener, std::memory_order_release);
}

void SilKitLinPort::require_role(LinRole role, const char* call) const
{
    if (role_ != role)
    {
        throw std::logic_error(std::string("vehicle_sil::lin::SilKitLinPort: ") + call + " is a function of a " +
                               role_name(role) + " port, and this port is a " + role_name(role_) +
                               " port; make a port with LinRole::" + (role == LinRole::Master ? "Master" : "Slave") +
                               " for it");
    }
}

void SilKitLinPort::require_frame(LinId id, std::size_t data_length, const char* call)
{
    if (!valid_lin_frame(id, data_length))
    {
        throw std::invalid_argument(std::string("vehicle_sil::lin::SilKitLinPort: ") + call + " with id " +
                                    std::to_string(id) + " and " + std::to_string(data_length) +
                                    " bytes: a LIN id is 0..63 and a frame has 1..8 bytes");
    }
}

void SilKitLinPort::open()
{
    if (role_ == LinRole::Master)
    {
        // No frame responses here: send_header() configures each identifier when it first
        // sends its header.
        silkit_lin::LinControllerConfig config;
        config.controllerMode = silkit_lin::LinControllerMode::Master;
        config.baudRate = kBaudRate;
        controller_.Init(config);
    }
    else
    {
        experimental_lin::LinControllerDynamicConfig config;
        config.controllerMode = silkit_lin::LinControllerMode::Slave;
        config.baudRate = kBaudRate;
        experimental_lin::InitDynamic(&controller_, config);
        // In the same SIL Kit handler as InitDynamic(), so no header arrives in between.
        header_handler_ = experimental_lin::AddFrameHeaderHandler(
            &controller_, [this](silkit_lin::ILinController*, const experimental_lin::LinFrameHeaderEvent& event) {
                if (auto* listener = listener_.load(std::memory_order_acquire))
                {
                    listener->on_header(from_silkit_time(event.timestamp), event.id);
                }
            });
    }
}

void SilKitLinPort::send_header(SimTime /*now*/, LinId id, std::uint8_t data_length, LinChecksumModel checksum_model)
{
    require_role(LinRole::Master, "send_header()");
    require_frame(id, data_length, "send_header()");
    // The master receives a slave's response only for an identifier configured for reception;
    // SIL Kit compares the response's length and checksum model with this configuration.
    if (!configured_.test(id))
    {
        silkit_lin::LinFrameResponse response;
        response.frame.id = id;
        response.frame.checksumModel = to_silkit(checksum_model);
        response.frame.dataLength = data_length;
        response.responseMode = silkit_lin::LinFrameResponseMode::Rx;
        controller_.SetFrameResponse(response);
        configured_.set(id);
    }
    // With no responder configured and no dynamic-response slave on the bus, SIL Kit reports
    // LIN_RX_NO_RESPONSE synchronously from here.
    controller_.SendFrameHeader(id);
}

void SilKitLinPort::send_frame(SimTime /*now*/, const LinFrame& frame)
{
    require_role(LinRole::Master, "send_frame()");
    require_frame(frame.id, frame.data_length, "send_frame()");
    controller_.SendFrame(to_silkit(frame), silkit_lin::LinFrameResponseType::MasterResponse);
}

void SilKitLinPort::send_go_to_sleep(SimTime /*now*/)
{
    require_role(LinRole::Master, "send_go_to_sleep()");
    // Sends the go-to-sleep command and puts the controller to sleep.
    controller_.GoToSleep();
}

void SilKitLinPort::respond(SimTime /*header_time*/, const LinFrame& frame)
{
    require_role(LinRole::Slave, "respond()");
    experimental_lin::SendDynamicResponse(&controller_, to_silkit(frame));
}

void SilKitLinPort::send_wakeup(SimTime /*now*/)
{
    if (role_ == LinRole::Slave)
    {
        // The slave's controller is always operational. Wakeup() sends the pulse and switches
        // the controller to operational, and warns if it already is: switch it to sleep first.
        // Nothing is received in between: SIL Kit runs the step on the thread that delivers
        // messages.
        controller_.GoToSleepInternal();
    }
    controller_.Wakeup(); // sends the pulse and makes the controller operational
}

void SilKitLinPort::resume()
{
    if (role_ == LinRole::Master)
    {
        controller_.WakeupInternal();
    }
}

void SilKitLinPort::on_frame_status(const silkit_lin::LinFrameStatusEvent& event)
{
    auto* listener = listener_.load(std::memory_order_acquire);
    if (listener == nullptr)
    {
        return;
    }
    if (role_ == LinRole::Slave)
    {
        // TX statuses are this slave's own responses; error statuses carry no valid data; the
        // go-to-sleep command goes to on_go_to_sleep().
        if (event.status != silkit_lin::LinFrameStatus::LIN_RX_OK || is_go_to_sleep_frame(event.frame))
        {
            return;
        }
        listener->on_response(from_silkit_time(event.timestamp), from_silkit(event.frame), LinResultStatus::Ok);
        return;
    }
    listener->on_response(from_silkit_time(event.timestamp), from_silkit(event.frame), to_result_status(event.status));
}

} // namespace vehicle_sil::lin
