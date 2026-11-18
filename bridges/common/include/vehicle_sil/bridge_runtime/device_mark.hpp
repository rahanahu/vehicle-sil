// SPDX-License-Identifier: MIT
//
// The mark a bridge puts on the device it opened (an Ethernet interface, a CAN channel), so that
// the vehicle-sil bridges that open the same device find each other: two bridges on one device
// receive each other's frames, and two networks (or two runs) mix there. The mark excludes
// nobody; every bridge places one, and each counts the marks of the others.
//
//   Linux    a Unix socket in the abstract namespace, bound to
//            "vehicle-sil/device/<kind>/<identity>/<pid>/<random>" (no file; gone with the process;
//            per network namespace, as the interfaces are). Counting reads /proc/net/unix.
//   Windows  a named mutex "Global\vehicle-sil-device-<hash>-<n>" (n from 0 to 15, the first free
//            one; "Local\" where this user may not create global names), never owned. Counting
//            opens the other numbers and closes them at once. The process ids are not known.
//   Others   no mark (marked() is false).
//
// docs/bridge-contract.md, "Device in use by another bridge", says what the marks find and what they miss.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::bridge_runtime {

/// The kinds of device ("<kind>" of the mark): the same device has the same kind and identity in
/// every bridge, whatever its backend.
inline constexpr std::string_view kEthernetDevice = "ethernet";
inline constexpr std::string_view kCanDevice = "can";

/// The marks of other bridges on one device, as one look found them.
struct DevicePeers
{
    /// How many marks (other than the one who looks).
    std::size_t count = 0;
    /// Their process ids, ascending, each once. Linux only (empty elsewhere); an id is the one in
    /// the PID namespace of the bridge that placed the mark.
    std::vector<std::uint32_t> pids;

    bool operator==(const DevicePeers&) const = default;
};

/// The mark of this process on one device. It stays while the object lives.
///
/// Threading: one object is used from one thread at a time.
class DeviceMark
{
public:
    /// Places the mark of the device `identity` of `kind` (kEthernetDevice, kCanDevice). It does not
    /// throw where the OS refuses: marked() is then false and why_not() says why.
    [[nodiscard]] static DeviceMark place(std::string_view kind, std::string_view identity);

    DeviceMark(DeviceMark&&) noexcept;
    DeviceMark& operator=(DeviceMark&&) noexcept;
    ~DeviceMark();

    /// The hash of the kind and the identity ("device=" of the link line): the same for every
    /// bridge on the device.
    [[nodiscard]] const std::string& device() const noexcept;
    [[nodiscard]] bool marked() const noexcept;
    /// Why the mark could not be placed (a sentence), or empty.
    [[nodiscard]] const std::string& why_not() const noexcept;
    /// The marks of the other bridges on the device now (this one left out).
    [[nodiscard]] DevicePeers others() const;

private:
    struct Impl;
    explicit DeviceMark(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

/// The marks on a device now, placing none (for the lists of the bridges).
[[nodiscard]] DevicePeers look_for_marks(std::string_view kind, std::string_view identity);

/// What a list says of a device with marks: "in use by a vehicle-sil bridge (process 4321)",
/// "in use by 2 vehicle-sil bridges (processes 4321, 4400)", "in use by a vehicle-sil bridge"
/// (no process ids, Windows). Empty where there is no mark.
[[nodiscard]] std::string in_use_text(const DevicePeers& peers);

/// One placed mark, counted again on each count(): the keys it adds to the link line, and a
/// warning whenever more bridges share the device than at the count before.
class DeviceShareWatch
{
public:
    /// `device_text`: how the warnings name the device ("interface 'eth0'"). Counts once; a mark
    /// that could not be placed is one warning.
    DeviceShareWatch(DeviceMark mark, std::string device_text);

    /// Counts the other bridges' marks again.
    void count();
    /// " device=<hash> marked=<0|1> shared=<n>" and " peers=<pid>,<pid>" where process ids are
    /// known, as of the last count(): to put after the link line's first key.
    [[nodiscard]] std::string link_keys() const;
    /// The warnings since the last call.
    [[nodiscard]] std::vector<std::string> take_warnings();

private:
    DeviceMark mark_;
    std::string device_text_;
    DevicePeers peers_;
    std::vector<std::string> warnings_;
};

namespace detail {

/// FNV-1a (64 bits) of "<kind>/<identity>", 16 lowercase hex digits.
[[nodiscard]] std::string device_hash(std::string_view kind, std::string_view identity);

/// The name of a Linux mark up to the process id: "vehicle-sil/device/<kind>/<identity>/". The
/// identity is replaced by device_hash() where it has a character other than ASCII letters,
/// digits and "._:@+-", or where the whole name could pass the 107 bytes of an abstract name.
[[nodiscard]] std::string linux_mark_prefix(std::string_view kind, std::string_view identity);

/// "<prefix><pid>/<random as 16 hex digits>".
[[nodiscard]] std::string linux_mark_name(std::string_view prefix, std::uint32_t pid, std::uint64_t random);

/// The marks with `prefix` in the text of /proc/net/unix (abstract names show as "@<name>"),
/// leaving out `own` (the whole name of the one who looks; empty: none).
[[nodiscard]] DevicePeers read_proc_net_unix(std::string_view text, std::string_view prefix, std::string_view own);

/// "<space>vehicle-sil-device-<hash>-<number>", `space` "Global\" or "Local\".
[[nodiscard]] std::string windows_mark_name(std::string_view space, std::string_view hash, unsigned number);

/// How many numbers a device has on Windows (marks beyond them are not placed).
inline constexpr unsigned kWindowsMarks = 16;

/// The text of DeviceShareWatch::link_keys().
[[nodiscard]] std::string format_device_keys(std::string_view device, bool marked, const DevicePeers& peers);

/// The warning when more bridges share the device `device_text` now than before, or nullopt.
[[nodiscard]] std::optional<std::string> shared_warning(std::string_view device_text, const DevicePeers& before,
                                                        const DevicePeers& now);

} // namespace detail

} // namespace vehicle_sil::bridge_runtime
