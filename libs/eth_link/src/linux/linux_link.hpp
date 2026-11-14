// SPDX-License-Identifier: MIT
//
// Linux only. What the TAP link and the packet link share: one descriptor that frames are read
// from and written to, the receive thread, the watch on the interface (up or down, still there),
// and the contract of ExternalLink (external_link.hpp).
#pragma once

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/eth_link/os_links.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace vehicle_sil::eth_link::detail {

/// A file descriptor that is closed with its owner.
class Fd
{
public:
    Fd() noexcept = default;
    explicit Fd(int fd) noexcept : fd_(fd) {}
    ~Fd() { reset(); }
    Fd(Fd&& other) noexcept : fd_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    int release() noexcept
    {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

/// The interface's flags (IFF_UP, IFF_RUNNING, ...) and link type (ARPHRD_*), or nullopt if there
/// is no interface `name`.
struct InterfaceState
{
    unsigned flags = 0;
    unsigned link_type = 0;
};
[[nodiscard]] std::optional<InterfaceState> interface_state(const std::string& name);
/// The names of this computer's interfaces.
[[nodiscard]] std::vector<std::string> interface_names();
/// The path of this program ("/proc/self/exe"), or empty.
[[nodiscard]] std::string executable_path();

class LinuxLink : public ExternalLink
{
public:
    void start(EventHandler handler) final;
    void stop() noexcept final;
    TransmitResult transmit(std::span<const std::uint8_t> frame) final;
    /// Neither a TAP device nor a packet socket gives the link the frames it sent itself (the
    /// test ethernet_bridge.netns.* holds this against the kernel).
    [[nodiscard]] bool echoes_own_transmissions() const noexcept final { return false; }
    [[nodiscard]] std::string last_error() const final;

protected:
    explicit LinuxLink(std::string name);
    /// The classes that derive call stop() in their destructor (the receive thread calls their
    /// virtual functions).
    ~LinuxLink() override = default;

    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    /// Opens the interface. Returns the descriptor (non-blocking) that frames are read from and
    /// written to. Throws LinkError.
    virtual Fd open() = 0;

    enum class ReadStatus
    {
        Frame, ///< `frame` is one received frame
        Skip,  ///< nothing to deliver from this read; read again
        Empty, ///< nothing more to read now
        Error, ///< reading failed with `error` (errno)
    };
    struct ReadResult
    {
        ReadStatus status = ReadStatus::Empty;
        std::span<const std::uint8_t> frame{};
        int error = 0;
        /// Frame: the OS of this computer sent it and has not filled in its TCP or UDP checksum.
        bool checksum_incomplete = false;
        /// Frame: the copy of a frame sent on this interface, not one that arrived there.
        bool sent_here = false;
    };
    /// Reads one frame into `buffer` (kReadBufferBytes). The frame may lie anywhere in it.
    virtual ReadResult read_frame(int fd, std::span<std::uint8_t> buffer) = 0;
    /// Writes one frame without blocking: the bytes written, or -1 with errno.
    virtual long write_frame(int fd, std::span<const std::uint8_t> frame) = 0;
    /// The received frames the OS dropped since the last call.
    virtual std::uint64_t take_overruns(int fd) noexcept;

    /// Room for the longest frame the bridge may pass (65535 bytes) and 4 bytes to put a VLAN tag
    /// back. A longer frame is cut, and is still longer than every limit of the frame filter.
    static constexpr std::size_t kReadBufferBytes = 65536 + 4;

private:
    void run();
    /// Looks at the interface: reports a change of the link state and the OS's drops. False once
    /// the interface is gone (the Fault is reported).
    bool watch(int fd);
    void fault(const std::string& detail);
    void set_last_error(std::string text);

    const std::string name_;

    std::mutex lifecycle_mutex_; // one start() or stop() at a time
    /// Held shared by transmit() while it writes, exclusively only while the descriptor is set or
    /// closed. Never held while the handler runs.
    mutable std::shared_mutex fd_mutex_;
    Fd fd_;
    Fd wake_; // eventfd: written to end the receive thread
    unsigned interface_index_ = 0;
    std::atomic<bool> faulted_{false};
    EventHandler handler_;
    std::thread thread_;
    LinkState reported_state_ = LinkState::Unknown; // receive thread only
    bool checksum_warned_ = false;                  // receive thread only
    std::vector<std::uint8_t> buffer_;              // receive thread only

    mutable std::mutex error_mutex_;
    std::string last_error_;
};

} // namespace vehicle_sil::eth_link::detail
