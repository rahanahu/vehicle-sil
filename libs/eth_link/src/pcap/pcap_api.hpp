// SPDX-License-Identifier: MIT
//
// The part of the pcap library (Npcap's wpcap.dll on Windows, libpcap on Linux) the pcap link
// uses, loaded at run time: vehicle-sil is built without the library's headers and without its
// import library, and ships neither.
//
// The declarations below are vehicle-sil's own, written against the library's documented binary
// interface (function names, argument types, the three structures the link reads, the numbers
// of the results). They are checked against a real libpcap by the test ethernet_bridge.netns.pcap
// on Linux, and against a fake library that is built from these same declarations everywhere
// (tests/eth_link/fake_pcap). Against a real Npcap they are not checked by a test.
#pragma once

#include "pcap_errors.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace vehicle_sil::eth_link::detail {

/// An open capture (the library's pcap_t). Only ever a pointer.
struct PcapHandle;
/// An address of a device in the library's list. Not read.
struct PcapAddress;

/// One entry of the library's device list (the library's pcap_if_t).
struct PcapInterface
{
    PcapInterface* next;
    char* name;
    char* description; ///< may be null
    PcapAddress* addresses;
    std::uint32_t flags; ///< kPcapIf*
};

inline constexpr std::uint32_t kPcapIfLoopback = 0x01;
inline constexpr std::uint32_t kPcapIfUp = 0x02;
inline constexpr std::uint32_t kPcapIfRunning = 0x04;
inline constexpr std::uint32_t kPcapIfWireless = 0x08;
inline constexpr std::uint32_t kPcapIfConnectionMask = 0x30;
inline constexpr std::uint32_t kPcapIfConnected = 0x10;
inline constexpr std::uint32_t kPcapIfDisconnected = 0x20;

/// What the library says about one received packet (the library's pcap_pkthdr). The time is a
/// struct timeval, which is two longs on 64 bit Linux and on Windows alike (of 8 and of 4 bytes).
struct PcapPacketHeader
{
    long seconds;
    long microseconds;
    std::uint32_t captured_bytes; ///< how many bytes of the packet are there
    std::uint32_t wire_bytes;     ///< how long the packet was
};

/// The library's counters (its pcap_stat): three numbers everywhere; room for the further ones
/// some builds for Windows have behind them.
struct PcapStatistics
{
    unsigned received;
    unsigned dropped; ///< by the library or the OS, because the link did not read fast enough
    unsigned dropped_by_interface;
    unsigned more[5];
};

#if defined(_WIN64) || (defined(__linux__) && defined(__x86_64__))
static_assert(sizeof(PcapPacketHeader) == 2 * sizeof(long) + 8);
static_assert(sizeof(PcapInterface) == 40);
#endif

/// Room for the text a function writes on failure (the library asks for 256 bytes).
inline constexpr std::size_t kPcapErrorTextBytes = 1024;
/// The link type of Ethernet.
inline constexpr int kPcapLinkTypeEthernet = 1;

/// How the receive thread waits for packets: the library offers a different way on each OS.
enum class PcapWaitKind
{
    Event,      ///< Windows: pcap_getevent(), an event handle
    Descriptor, ///< POSIX: pcap_get_selectable_fd(), a file descriptor
};

/// The functions, under vehicle-sil's names. All are set after bind_pcap_api() reported nothing
/// missing, except the one of the other PcapWaitKind.
struct PcapApi
{
    const char* (*lib_version)() = nullptr;
    int (*findalldevs)(PcapInterface** list, char* error) = nullptr;
    void (*freealldevs)(PcapInterface* list) = nullptr;

    PcapHandle* (*create)(const char* source, char* error) = nullptr;
    int (*set_snaplen)(PcapHandle*, int bytes) = nullptr;
    int (*set_promisc)(PcapHandle*, int on) = nullptr;
    int (*set_timeout)(PcapHandle*, int milliseconds) = nullptr;
    int (*set_immediate_mode)(PcapHandle*, int on) = nullptr;
    int (*activate)(PcapHandle*) = nullptr;
    int (*datalink)(PcapHandle*) = nullptr;
    int (*setnonblock)(PcapHandle*, int on, char* error) = nullptr;
    void (*close)(PcapHandle*) = nullptr;

    /// 1: a packet; 0: none now; negative: an error (geterr() tells).
    int (*next_ex)(PcapHandle*, PcapPacketHeader** header, const unsigned char** bytes) = nullptr;
    /// 0: sent; else an error.
    int (*sendpacket)(PcapHandle*, const unsigned char* bytes, int size) = nullptr;
    char* (*geterr)(PcapHandle*) = nullptr;
    int (*stats)(PcapHandle*, PcapStatistics*) = nullptr;

    void* (*getevent)(PcapHandle*) = nullptr;       ///< PcapWaitKind::Event
    int (*get_selectable_fd)(PcapHandle*) = nullptr; ///< PcapWaitKind::Descriptor
};

/// Fills the table from `resolve` (the address of the function with that name, or null) and
/// returns the names `resolve` did not have.
[[nodiscard]] std::pair<PcapApi, std::vector<std::string>> bind_pcap_api(const std::function<void*(const char*)>& resolve,
                                                                        PcapWaitKind wait);

/// Why PcapWaiter::wait() returned.
enum class PcapWake
{
    Stop,     ///< request_stop() was called
    Ready,    ///< a packet may be there
    TimedOut, ///< the limit passed
};

/// Waits for packets on one open capture, and can be told to stop waiting, from another thread.
class PcapWaiter
{
public:
    virtual ~PcapWaiter() = default;
    /// Waits until a packet may be there, `limit` has passed, or request_stop() was called.
    virtual PcapWake wait(std::chrono::milliseconds limit) = 0;
    /// Waits for `time` whether or not packets are there. Returns true once request_stop() was called.
    virtual bool pause(std::chrono::milliseconds time) = 0;
    virtual void request_stop() noexcept = 0;
};

/// What the OS says about a network interface.
enum class PcapInterfaceState
{
    Up,   ///< switched on, and its link is up
    Down, ///< switched off, or its link is down
    Gone, ///< removed or renamed
};

/// Asks the OS about the interface of one open capture while the link runs: the library does not
/// tell when the interface goes down or away.
class PcapInterfaceWatch
{
public:
    virtual ~PcapInterfaceWatch() = default;
    virtual PcapInterfaceState look() = 0;
};

/// The OS's name of a network connection, by the identifier its pcap device name contains
/// (Windows: "{4D36E972-...}" and "Ethernet 2").
struct PcapConnectionName
{
    std::string id;
    std::string name;
};

/// The loaded library. Unloaded with the last owner.
class PcapLibrary
{
public:
    virtual ~PcapLibrary() = default;
    PcapLibrary(const PcapLibrary&) = delete;
    PcapLibrary& operator=(const PcapLibrary&) = delete;

    [[nodiscard]] const PcapApi& api() const noexcept { return api_; }
    [[nodiscard]] PcapFlavor flavor() const noexcept { return flavor_; }
    /// The file it was loaded from, as it was asked for.
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    /// What the library says it is (pcap_lib_version()).
    [[nodiscard]] std::string version() const;

    /// Throws LinkError.
    [[nodiscard]] virtual std::unique_ptr<PcapWaiter> make_waiter(PcapHandle* handle) const = 0;
    /// Null where the OS cannot be asked about the device `name` (a pcap name): on Windows (not
    /// done yet), and for a device that is not a network interface of the OS.
    [[nodiscard]] virtual std::unique_ptr<PcapInterfaceWatch> make_watch(const std::string& name) const = 0;
    /// Empty where pcap's device names are the OS's own names (Linux).
    [[nodiscard]] virtual std::vector<PcapConnectionName> connection_names() const = 0;
    /// Any function of the library by name, or null (the tests reach the fake library's controls with it).
    [[nodiscard]] virtual void* symbol(const char* name) const noexcept = 0;
    /// The path of this program for the message about capabilities, or empty.
    [[nodiscard]] virtual std::string executable_path() const = 0;

protected:
    PcapLibrary(PcapFlavor flavor, std::string path) : flavor_(flavor), path_(std::move(path)) {}
    /// Fills api(); throws LinkError naming the functions the library lacks.
    void bind(PcapWaitKind wait);

private:
    PcapApi api_{};
    const PcapFlavor flavor_;
    const std::string path_;
};

/// Loads the pcap library of this operating system. `library`: empty for the installed one
/// (Windows: wpcap.dll in the Npcap directory of the system directory, by its full path, and
/// nowhere else; elsewhere: libpcap.so.1, then libpcap.so.0.8, as the system's loader finds
/// them); else the file to load instead. Throws LinkError (LibraryMissing).
[[nodiscard]] std::shared_ptr<PcapLibrary> load_pcap_library(const std::string& library);

} // namespace vehicle_sil::eth_link::detail
