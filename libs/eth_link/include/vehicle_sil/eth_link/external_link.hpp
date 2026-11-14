// SPDX-License-Identifier: MIT
//
// The external side of an Ethernet bridge: one Ethernet interface outside SIL Kit (a TAP device,
// a network adapter, a fake in tests). Frames are bytes from the destination MAC address to the
// end of the payload, without the frame check sequence. No SIL Kit.
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace vehicle_sil::eth_link {

/// What an external link reports.
enum class LinkEventKind
{
    Frame,     ///< A frame received from the interface (`frame`).
    Overrun,   ///< `count` received frames were lost before the link could read them (counted by the OS).
    LinkState, ///< The interface's link went up or down (`state`).
    /// The link stopped working (interface removed, reading fails); `detail` tells why. No frames
    /// follow; transmit() fails from then on.
    Fault,
    /// Something the user should know that does not stop the link; `detail` tells what.
    Warning,
};

/// Whether the interface's link is up, as far as the link can tell.
enum class LinkState
{
    Unknown, ///< not reported (yet), or the link cannot tell
    Up,
    Down,
};

/// "link up", "link down", "link state unknown".
[[nodiscard]] std::string_view to_string(LinkState state) noexcept;

struct LinkEvent
{
    LinkEventKind kind = LinkEventKind::Frame;
    /// Valid for Frame, during the handler call only.
    std::span<const std::uint8_t> frame{};
    /// Valid for Overrun.
    std::uint64_t count = 0;
    /// Valid for LinkState.
    LinkState state = LinkState::Unknown;
    /// Valid for Fault and Warning, during the handler call only.
    std::string_view detail{};
};

enum class TransmitResult
{
    Ok,
    QueueFull, ///< The interface's transmit queue is full; the frame was not sent.
    Failed,    ///< Any other error (link closed, interface gone); see last_error().
};

/// Why a link could not start.
enum class LinkErrorKind
{
    NoSuchInterface,
    PermissionDenied,
    LibraryMissing, ///< a library the link loads at run time is not installed
    NotEthernet,    ///< the interface does not carry Ethernet frames
    Other,
};

/// "no such interface", "permission denied", "library missing", "not Ethernet", "other".
[[nodiscard]] std::string_view to_string(LinkErrorKind kind) noexcept;

/// Thrown by ExternalLink::start(). what() says what happened and how to fix it.
class LinkError : public std::runtime_error
{
public:
    LinkError(LinkErrorKind kind, const std::string& message);
    [[nodiscard]] LinkErrorKind kind() const noexcept { return kind_; }

private:
    LinkErrorKind kind_;
};

/// One external Ethernet interface.
///
/// Threading: the handler is called from one thread at a time (the link's receive thread, and
/// the thread that calls stop()). transmit() may be called from any thread, also concurrently
/// with the handler and with itself.
///
/// transmit() must not block, must not throw and must not wait for the receive thread; and while
/// the link calls the handler, it must not hold a lock that transmit() takes. The bridge calls
/// transmit() on SIL Kit's receive thread while SIL Kit holds the lock of its frame handlers, and
/// the link's handler calls SendFrame(), which takes that same lock: a link lock held across the
/// handler and taken in transmit() deadlocks the two threads. (The bridge still counts an
/// exception from transmit() as a failed transmission instead of letting it through.)
///
/// The receive time of a frame is not reported: the OS's clock is not the simulation's.
class ExternalLink
{
public:
    using EventHandler = std::function<void(const LinkEvent&)>;

    virtual ~ExternalLink() = default;

    /// Opens the interface and starts delivering events to `handler`. Throws LinkError.
    virtual void start(EventHandler handler) = 0;
    /// Stops delivering events. Before it returns it may report, from the calling thread, the
    /// frames the OS dropped since the last report (Overrun). When it returns, the handler is no
    /// longer running or called. Safe to call when not started.
    virtual void stop() noexcept = 0;
    /// Sends one frame (destination MAC to the end of the payload, no FCS). The frame is only
    /// read during the call. Does not block: a full transmit queue is QueueFull.
    virtual TransmitResult transmit(std::span<const std::uint8_t> frame) = 0;
    /// Whether the frames transmit() sends come back to the handler as received frames (some
    /// interfaces deliver what was sent on them). Fixed for the link's lifetime.
    ///
    /// The bridge's loop rule depends on it. With false, a frame from the outside whose source
    /// MAC the bridge saw from SIL Kit cannot be the bridge's own: the address is on both sides,
    /// and the bridge drops its frames and warns. With true, the bridge takes such a frame for
    /// its own coming back and drops it quietly, so it cannot tell a second bridge between the
    /// same networks, or one address used on both sides, from its own frames.
    [[nodiscard]] virtual bool echoes_own_transmissions() const noexcept = 0;
    /// What the link is, for the bridge's start line: kind, name, real or virtual
    /// (e.g. "tap 'vsil0' (virtual)").
    [[nodiscard]] virtual std::string describe() const = 0;
    [[nodiscard]] virtual std::string last_error() const = 0;
};

} // namespace vehicle_sil::eth_link
