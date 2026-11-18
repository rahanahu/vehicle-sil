// SPDX-License-Identifier: MIT
//
// The operations SocketCanPort needs of the operating system, in vehicle-sil's own types: one raw
// CAN socket on one interface, and what the kernel tells of the interface. LinuxCanDevice calls the
// kernel; the tests give one that plays what the kernel does (frames, error frames, errno values).
#pragma once

#include "vehicle_sil/socketcan/can_interfaces.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace vehicle_sil::socketcan {

/// A classic CAN frame as the socket gives and takes it (struct can_frame).
struct RawFrame
{
    std::uint32_t can_id = 0; ///< with the flags CAN_EFF_FLAG, CAN_RTR_FLAG, CAN_ERR_FLAG
    std::uint8_t length = 0;  ///< can_dlc: 0..8
    std::array<std::uint8_t, 8> data{};
    /// Received: the frame this socket sent itself (MSG_CONFIRM of recvmsg).
    bool confirm = false;
    /// Received: a program of this computer sent it (MSG_DONTROUTE of recvmsg).
    bool sent_here = false;
};

enum class WaitResult
{
    Readable, ///< a frame or an error can be read
    Timeout,
    Woken, ///< wake() was called
};

enum class ReadStatus
{
    Frame,
    Empty, ///< nothing more to read now
    Error, ///< `error` (errno)
};

struct ReadResult
{
    ReadStatus status = ReadStatus::Empty;
    RawFrame frame;
    int error = 0;
};

class CanDevice
{
public:
    virtual ~CanDevice() = default;

    /// Opens a raw CAN socket on the interface: classic frames only, the frames this socket sends
    /// come back marked (CAN_RAW_RECV_OWN_MSGS), error frames are received (CAN_RAW_ERR_FILTER),
    /// non-blocking. Throws std::runtime_error with the reason and the way to fix it.
    virtual void open() = 0;
    /// Waits for something to read, for at most `timeout`.
    virtual WaitResult wait(std::chrono::milliseconds timeout) = 0;
    /// Reads one frame, or the error the socket holds (ENETDOWN once the interface went down,
    /// ENODEV once it is gone).
    virtual ReadResult read() = 0;
    /// Sends one frame without blocking: 0, or the errno (ENOBUFS and EAGAIN: the queue is full,
    /// ENETDOWN: the interface is down, ENODEV and ENXIO: it is gone). Any thread, also while
    /// wait() or read() run.
    virtual int write(const RawFrame& frame) = 0;
    /// What the kernel tells of the interface now: nullopt where it is gone (removed, or renamed);
    /// throws std::runtime_error where the kernel cannot be asked.
    virtual std::optional<CanInterfaceInfo> interface() = 0;
    /// Makes a running or the next wait() return Woken. Any thread.
    virtual void wake() noexcept = 0;
    /// Closes the socket. Idempotent.
    virtual void close() noexcept = 0;
};

/// The device on the interface `name` of this computer.
[[nodiscard]] std::unique_ptr<CanDevice> make_linux_can_device(std::string name);

} // namespace vehicle_sil::socketcan
