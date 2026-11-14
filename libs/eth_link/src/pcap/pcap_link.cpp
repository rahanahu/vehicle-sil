// SPDX-License-Identifier: MIT
#include "pcap_link.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>
#include <utility>

namespace vehicle_sil::eth_link::detail {
namespace {

/// The longest the receive thread waits before it looks at the stop request and the counters.
constexpr auto kWaitLimit = std::chrono::milliseconds(100);
/// How often the receive thread reads the library's count of dropped frames and asks the OS
/// about the interface.
constexpr auto kWatchInterval = std::chrono::milliseconds(250);
/// Frames read in a row before the thread looks at the stop request again.
constexpr int kFramesPerWake = 64;
/// Wakes in a row without a frame after which the thread waits a moment before it reads again
/// (a library whose "packets are there" sign stays set must not make the thread spin).
constexpr int kEmptyWakesBeforePause = 8;
constexpr auto kEmptyWakePause = std::chrono::milliseconds(10);
/// Waits that ran out with frames there, while the library's sign has never brought a frame,
/// after which the link warns that the sign does not work (once).
constexpr int kUnsignalledReadsBeforeWarning = 3;
/// The library's read timeout. Not used for waiting (the link reads without blocking); it is set
/// because a capture without one waits forever on some systems.
constexpr int kReadTimeoutMs = 100;
/// The most bytes of a frame the link asks for: one more than the longest frame the bridge may
/// pass (65535), so a longer frame is cut and is still longer than every limit of the frame filter.
constexpr int kSnapshotBytes = 65536;

std::string text_or_empty(const char* text)
{
    return text != nullptr ? std::string(text) : std::string();
}

/// The library's texts may end in a line break or a full stop; the messages add their own.
std::string trimmed(std::string text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.'))
    {
        text.pop_back();
    }
    return text;
}

bool same_ignoring_case(std::string_view a, std::string_view b) noexcept
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
    });
}

std::string_view without_braces(std::string_view text) noexcept
{
    if (text.size() >= 2 && text.front() == '{' && text.back() == '}')
    {
        text.remove_prefix(1);
        text.remove_suffix(1);
    }
    return text;
}

/// The identifier in a pcap device name of Windows ("\Device\NPF_{<this>}"), or empty.
std::string_view identifier_of(std::string_view name) noexcept
{
    const auto open = name.find('{');
    const auto close = name.rfind('}');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open + 2)
    {
        return {};
    }
    return name.substr(open + 1, close - open - 1);
}

/// A name as it is typed after --interface: in double quotes if it has a blank.
std::string as_typed(const std::string& name)
{
    return name.empty() || name.find(' ') != std::string::npos ? "\"" + name + "\"" : name;
}

std::string state_text(std::uint32_t flags)
{
    std::string text = (flags & kPcapIfUp) != 0 ? "up" : "down";
    if ((flags & kPcapIfConnectionMask) == kPcapIfConnected)
    {
        text += ", connected";
    }
    else if ((flags & kPcapIfConnectionMask) == kPcapIfDisconnected)
    {
        text += ", disconnected";
    }
    if ((flags & kPcapIfWireless) != 0)
    {
        text += ", wireless";
    }
    if ((flags & kPcapIfLoopback) != 0)
    {
        text += ", loopback";
    }
    return text;
}

/// Closes a capture that start() could not finish opening.
class OpenGuard
{
public:
    OpenGuard(const PcapApi& api, PcapHandle* handle) noexcept : api_(api), handle_(handle) {}
    ~OpenGuard()
    {
        if (handle_ != nullptr)
        {
            api_.close(handle_);
        }
    }
    OpenGuard(const OpenGuard&) = delete;
    OpenGuard& operator=(const OpenGuard&) = delete;
    PcapHandle* release() noexcept { return std::exchange(handle_, nullptr); }

private:
    const PcapApi& api_;
    PcapHandle* handle_;
};

class PcapLink final : public ExternalLink
{
public:
    PcapLink(std::shared_ptr<PcapLibrary> library, PcapLinkOptions options) :
      library_(std::move(library)),
      api_(library_->api()),
      flavor_(library_->flavor()),
      options_(std::move(options)),
      described_(std::string(backend_name(flavor_)) + " '" + options_.name + "'")
    {
    }
    ~PcapLink() override { stop(); }

    void start(EventHandler handler) override;
    void stop() noexcept override;
    TransmitResult transmit(std::span<const std::uint8_t> frame) override;
    /// Linux: the library's socket does not get the frames it sent (held by the test
    /// ethernet_bridge.netns.pcap). Windows: not verified; "yes" keeps the bridge forwarding
    /// whether Npcap returns them or not, where "no" would take every returned frame for a
    /// second bridge and stop forwarding.
    [[nodiscard]] bool echoes_own_transmissions() const noexcept override { return flavor_ == PcapFlavor::Npcap; }
    [[nodiscard]] std::string describe() const override
    {
        const std::lock_guard lock(text_mutex_);
        return described_;
    }
    [[nodiscard]] std::string last_error() const override
    {
        const std::lock_guard lock(text_mutex_);
        return last_error_;
    }

private:
    /// Opens `device` for sending and receiving without blocking. Throws LinkError.
    PcapHandle* open(const PcapDevice& device, std::vector<std::string>& warnings);
    void run();
    void fault(const std::string& detail);
    void warn(const std::string& detail);
    void report_overruns();
    /// Asks the OS about the interface: reports a change of the link state, and a Fault once the
    /// interface is gone.
    void look_at_interface();
    [[nodiscard]] std::string gone_text() const;
    /// The frames the library dropped since the last call.
    std::uint64_t take_overruns() noexcept;
    void set_last_error(std::string text)
    {
        const std::lock_guard lock(text_mutex_);
        last_error_ = std::move(text);
    }

    const std::shared_ptr<PcapLibrary> library_;
    const PcapApi& api_;
    const PcapFlavor flavor_;
    const PcapLinkOptions options_;

    std::mutex lifecycle_mutex_; // one start() or stop() at a time
    /// Held for every call on the capture (the library does not promise that one capture may be
    /// used from two threads at once) and while it is set or closed. Never held while the handler
    /// runs, and only around calls that do not block.
    std::mutex handle_mutex_;
    PcapHandle* handle_ = nullptr;
    unsigned dropped_seen_ = 0; // under handle_mutex_
    std::unique_ptr<PcapWaiter> waiter_;
    std::unique_ptr<PcapInterfaceWatch> watch_;     // receive thread only; may be null
    LinkState reported_state_ = LinkState::Unknown; // receive thread only
    std::atomic<bool> faulted_{false};
    EventHandler handler_;
    std::thread thread_;
    std::vector<std::string> start_warnings_; // receive thread only
    std::vector<std::uint8_t> buffer_;        // receive thread only

    mutable std::mutex text_mutex_;
    std::string last_error_;
    std::string described_;
};

PcapHandle* PcapLink::open(const PcapDevice& device, std::vector<std::string>& warnings)
{
    const std::string& shown = display_name(device);
    const std::string executable = library_->executable_path();
    char error[kPcapErrorTextBytes] = {};
    PcapHandle* const handle = api_.create(device.name.c_str(), error);
    if (handle == nullptr)
    {
        throw pcap_open_failed(flavor_, shown, "pcap_create", -1, trimmed(error), executable);
    }
    OpenGuard guard(api_, handle);
    auto must = [&](int result, const char* step) {
        if (result != 0)
        {
            throw pcap_open_failed(flavor_, shown, step, result, trimmed(text_or_empty(api_.geterr(handle))), executable);
        }
    };
    must(api_.set_snaplen(handle, kSnapshotBytes), "pcap_set_snaplen");
    must(api_.set_promisc(handle, options_.promiscuous ? 1 : 0), "pcap_set_promisc");
    must(api_.set_timeout(handle, kReadTimeoutMs), "pcap_set_timeout");
    // Without it the library hands frames over in batches, up to the read timeout late.
    must(api_.set_immediate_mode(handle, 1), "pcap_set_immediate_mode");
    const int activated = api_.activate(handle);
    if (activated < 0)
    {
        must(activated, "pcap_activate");
    }
    if (activated > 0)
    {
        warnings.push_back(pcap_activate_warning(shown, activated, trimmed(text_or_empty(api_.geterr(handle)))));
    }
    if (const int type = api_.datalink(handle); type != kPcapLinkTypeEthernet)
    {
        throw pcap_not_ethernet(flavor_, shown, type);
    }
    if (api_.setnonblock(handle, 1, error) != 0)
    {
        throw pcap_open_failed(flavor_, shown, "pcap_setnonblock", -1, trimmed(error), executable);
    }
    return guard.release();
}

void PcapLink::start(EventHandler handler)
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (thread_.joinable())
    {
        return;
    }
    const auto devices = pcap_devices(*library_);
    const PcapDevice& device = devices[choose_pcap_device(devices, options_.name, flavor_)];
    std::vector<std::string> warnings;
    if ((device.flags & kPcapIfWireless) != 0)
    {
        warnings.push_back(pcap_wireless_warning(display_name(device)));
    }
    PcapHandle* const handle = open(device, warnings);
    OpenGuard guard(api_, handle);
    auto waiter = library_->make_waiter(handle);
    watch_ = library_->make_watch(device.name);
    reported_state_ = LinkState::Unknown;

    {
        const std::lock_guard lock(text_mutex_);
        last_error_.clear();
        described_ = std::string(backend_name(flavor_)) + " '" + display_name(device) + "' (";
        if (device.name != display_name(device))
        {
            described_ += device.name + ", ";
        }
        described_ += flavor_ == PcapFlavor::Npcap ? "network adapter through Npcap)" : "network interface through libpcap)";
    }
    handler_ = std::move(handler);
    buffer_.assign(static_cast<std::size_t>(kSnapshotBytes), 0);
    start_warnings_ = std::move(warnings);
    faulted_.store(false);
    waiter_ = std::move(waiter);
    {
        const std::lock_guard lock(handle_mutex_);
        handle_ = guard.release();
        dropped_seen_ = 0;
    }
    thread_ = std::thread([this] { run(); });
}

void PcapLink::stop() noexcept
{
    const std::lock_guard lifecycle(lifecycle_mutex_);
    if (!thread_.joinable())
    {
        return;
    }
    waiter_->request_stop();
    thread_.join();
    // The drops since the thread's last look, from this thread: the handler is not running.
    if (!faulted_.load())
    {
        try
        {
            report_overruns();
        }
        catch (...)
        {
        }
    }
    waiter_.reset();
    {
        const std::lock_guard lock(handle_mutex_);
        api_.close(handle_);
        handle_ = nullptr;
    }
    handler_ = nullptr;
}

TransmitResult PcapLink::transmit(std::span<const std::uint8_t> frame)
{
    std::string error;
    {
        const std::lock_guard lock(handle_mutex_);
        if (handle_ == nullptr)
        {
            error = "the link on '" + options_.name + "' is not started";
        }
        else if (faulted_.load())
        {
            return TransmitResult::Failed; // last_error() tells the fault
        }
        else if (frame.size() > static_cast<std::size_t>(INT_MAX))
        {
            error = "sending on '" + options_.name + "' failed: the frame is too long";
        }
        else
        {
            errno = 0;
            const int result = api_.sendpacket(handle_, frame.data(), static_cast<int>(frame.size()));
            if (result == 0)
            {
                return TransmitResult::Ok;
            }
            // The library has one result for every failure. Where it leaves the OS's error number
            // in place (libpcap on Linux does), a full transmit queue can be told from the rest.
            const int number = errno;
            if (number == ENOBUFS || number == EAGAIN || number == EWOULDBLOCK)
            {
                return TransmitResult::QueueFull;
            }
            const std::string why = trimmed(text_or_empty(api_.geterr(handle_)));
            error = "sending on '" + options_.name + "' failed: " + (why.empty() ? "result " + std::to_string(result) : why);
        }
    }
    set_last_error(std::move(error));
    return TransmitResult::Failed;
}

std::uint64_t PcapLink::take_overruns() noexcept
{
    const std::lock_guard lock(handle_mutex_);
    PcapStatistics statistics{};
    if (handle_ == nullptr || api_.stats(handle_, &statistics) != 0)
    {
        return 0;
    }
    // The library's count runs on from when the capture was opened, and wraps around.
    const unsigned grown = statistics.dropped - dropped_seen_;
    dropped_seen_ = statistics.dropped;
    return grown;
}

void PcapLink::report_overruns()
{
    if (const auto dropped = take_overruns(); dropped > 0)
    {
        LinkEvent event;
        event.kind = LinkEventKind::Overrun;
        event.count = dropped;
        handler_(event);
    }
}

std::string PcapLink::gone_text() const
{
    return "interface '" + options_.name + "' is gone (removed or renamed while the bridge ran)";
}

void PcapLink::look_at_interface()
{
    if (watch_ == nullptr)
    {
        return;
    }
    const PcapInterfaceState state = watch_->look();
    if (state == PcapInterfaceState::Gone)
    {
        fault(gone_text());
        return;
    }
    if (const LinkState now = state == PcapInterfaceState::Up ? LinkState::Up : LinkState::Down; now != reported_state_)
    {
        reported_state_ = now;
        LinkEvent event;
        event.kind = LinkEventKind::LinkState;
        event.state = now;
        handler_(event);
    }
}

void PcapLink::warn(const std::string& detail)
{
    LinkEvent event;
    event.kind = LinkEventKind::Warning;
    event.detail = detail;
    handler_(event);
}

void PcapLink::fault(const std::string& detail)
{
    set_last_error(detail);
    faulted_.store(true);
    LinkEvent event;
    event.kind = LinkEventKind::Fault;
    event.detail = detail;
    handler_(event);
}

void PcapLink::run()
{
    for (const auto& warning : start_warnings_)
    {
        warn(warning);
    }
    look_at_interface();
    auto watched = std::chrono::steady_clock::now();
    bool more = false; // the last round stopped at kFramesPerWake: read on without waiting
    int emptyWakes = 0;
    bool signalled = false;       // this round's wait ended by the library's sign, not by its limit
    bool signalledFrames = false; // a wake by the library's sign has brought a frame
    int unsignalledReads = 0;     // waits that ran out after which frames were there
    for (;;)
    {
        if (faulted_.load())
        {
            // Nothing more to read; wait for stop().
            if (waiter_->pause(std::chrono::hours(1)))
            {
                return;
            }
            continue;
        }
        const bool continued = more;
        if (continued)
        {
            if (waiter_->pause(std::chrono::milliseconds(0)))
            {
                return;
            }
        }
        else
        {
            const PcapWake wake = waiter_->wait(kWaitLimit);
            if (wake == PcapWake::Stop)
            {
                return;
            }
            signalled = wake == PcapWake::Ready;
        }
        // Read after a wait that ran out too, not only when the library says packets are there:
        // a library whose sign never comes up (an event of Npcap that is not set, a descriptor
        // that never gets readable) would otherwise receive nothing at all, and say nothing. One
        // read without blocking for every kWaitLimit costs nothing; with such a library the
        // frames are late by up to kWaitLimit, and the link says so once.
        int frames = 0;
        while (frames < kFramesPerWake)
        {
            int result = 0;
            std::size_t size = 0;
            std::string error;
            {
                const std::lock_guard lock(handle_mutex_);
                PcapPacketHeader* header = nullptr;
                const unsigned char* bytes = nullptr;
                result = api_.next_ex(handle_, &header, &bytes);
                if (result == 1 && header != nullptr && bytes != nullptr)
                {
                    // The library's bytes are good until the next call on the capture, and
                    // the handler runs without the lock: take a copy.
                    size = std::min<std::size_t>(header->captured_bytes, buffer_.size());
                    std::memcpy(buffer_.data(), bytes, size);
                }
                else if (result < 0)
                {
                    error = trimmed(text_or_empty(api_.geterr(handle_)));
                }
            }
            if (result == 0)
            {
                break;
            }
            if (result < 0)
            {
                // A read fails too when the interface was removed; the OS knows whether that is it.
                if (watch_ != nullptr && watch_->look() == PcapInterfaceState::Gone)
                {
                    fault(gone_text());
                }
                else
                {
                    fault("reading from '" + options_.name + "' failed: " + (error.empty() ? "result " + std::to_string(result) : error));
                }
                break;
            }
            ++frames;
            LinkEvent event;
            event.kind = LinkEventKind::Frame;
            event.frame = std::span<const std::uint8_t>(buffer_.data(), size);
            handler_(event);
        }
        more = frames == kFramesPerWake;
        if (signalled)
        {
            signalledFrames = signalledFrames || frames > 0;
            emptyWakes = frames == 0 ? emptyWakes + 1 : 0;
            if (emptyWakes > kEmptyWakesBeforePause && !faulted_.load() && waiter_->pause(kEmptyWakePause))
            {
                return;
            }
        }
        else
        {
            emptyWakes = 0;
            if (frames > 0 && !continued && !signalledFrames && !faulted_.load() &&
                ++unsignalledReads == kUnsignalledReadsBeforeWarning)
            {
                warn(pcap_no_wake_warning(flavor_, options_.name, kWaitLimit));
            }
        }
        if (!faulted_.load() && std::chrono::steady_clock::now() - watched >= kWatchInterval)
        {
            look_at_interface();
            if (!faulted_.load())
            {
                report_overruns();
            }
            watched = std::chrono::steady_clock::now();
        }
    }
}

} // namespace

std::pair<PcapApi, std::vector<std::string>> bind_pcap_api(const std::function<void*(const char*)>& resolve, PcapWaitKind wait)
{
    PcapApi api;
    std::vector<std::string> missing;
    auto bind = [&](auto& slot, const char* name) {
        void* const address = resolve(name);
        if (address == nullptr)
        {
            missing.emplace_back(name);
        }
        // An object pointer from the loader to a function pointer: what dlsym() and
        // GetProcAddress() are made for.
        slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(address);
    };
    bind(api.lib_version, "pcap_lib_version");
    bind(api.findalldevs, "pcap_findalldevs");
    bind(api.freealldevs, "pcap_freealldevs");
    bind(api.create, "pcap_create");
    bind(api.set_snaplen, "pcap_set_snaplen");
    bind(api.set_promisc, "pcap_set_promisc");
    bind(api.set_timeout, "pcap_set_timeout");
    bind(api.set_immediate_mode, "pcap_set_immediate_mode");
    bind(api.activate, "pcap_activate");
    bind(api.datalink, "pcap_datalink");
    bind(api.setnonblock, "pcap_setnonblock");
    bind(api.close, "pcap_close");
    bind(api.next_ex, "pcap_next_ex");
    bind(api.sendpacket, "pcap_sendpacket");
    bind(api.geterr, "pcap_geterr");
    bind(api.stats, "pcap_stats");
    if (wait == PcapWaitKind::Event)
    {
        bind(api.getevent, "pcap_getevent");
    }
    else
    {
        bind(api.get_selectable_fd, "pcap_get_selectable_fd");
    }
    return {api, std::move(missing)};
}

void PcapLibrary::bind(PcapWaitKind wait)
{
    auto [api, missing] = bind_pcap_api([this](const char* name) { return symbol(name); }, wait);
    if (!missing.empty())
    {
        const std::string version = api.lib_version != nullptr ? text_or_empty(api.lib_version()) : std::string();
        throw pcap_functions_missing(flavor_, path_, version, missing);
    }
    api_ = api;
}

std::string PcapLibrary::version() const
{
    return trimmed(text_or_empty(api_.lib_version()));
}

std::vector<PcapDevice> pcap_devices(const PcapLibrary& library)
{
    const PcapApi& api = library.api();
    char error[kPcapErrorTextBytes] = {};
    PcapInterface* list = nullptr;
    if (api.findalldevs(&list, error) != 0)
    {
        throw pcap_list_failed(library.flavor(), trimmed(error));
    }
    std::vector<PcapDevice> devices;
    for (const PcapInterface* entry = list; entry != nullptr; entry = entry->next)
    {
        if (entry->name == nullptr)
        {
            continue;
        }
        PcapDevice device;
        device.name = entry->name;
        device.description = text_or_empty(entry->description);
        device.flags = entry->flags;
        devices.push_back(std::move(device));
    }
    if (list != nullptr)
    {
        api.freealldevs(list);
    }
    const auto connections = library.connection_names();
    for (auto& device : devices)
    {
        const auto id = identifier_of(device.name);
        for (const auto& connection : connections)
        {
            if (!id.empty() && same_ignoring_case(id, without_braces(connection.id)))
            {
                device.connection = connection.name;
                break;
            }
        }
    }
    return devices;
}

const std::string& display_name(const PcapDevice& device) noexcept
{
    return device.connection.empty() ? device.name : device.connection;
}

std::size_t choose_pcap_device(std::span<const PcapDevice> devices, std::string_view wanted, PcapFlavor flavor)
{
    std::vector<std::size_t> found;
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        if (devices[i].name == wanted)
        {
            return i; // pcap names are one per device
        }
        const auto id = identifier_of(devices[i].name);
        if ((!devices[i].connection.empty() && devices[i].connection == wanted) ||
            (!id.empty() && same_ignoring_case(id, without_braces(wanted))))
        {
            found.push_back(i);
        }
    }
    if (found.size() == 1)
    {
        return found.front();
    }
    std::vector<std::string> names;
    if (found.empty())
    {
        for (const auto& device : devices)
        {
            names.push_back(as_typed(display_name(device)));
        }
        throw pcap_no_interface(flavor, wanted, names);
    }
    for (const auto index : found)
    {
        names.push_back(devices[index].name);
    }
    throw pcap_ambiguous_interface(flavor, wanted, names);
}

std::string format_pcap_devices(std::span<const PcapDevice> devices, PcapFlavor flavor, std::string_view version)
{
    std::string text = "interfaces " + std::string(library_name(flavor)) + " offers";
    if (!version.empty())
    {
        text += " (" + std::string(version) + ")";
    }
    text += "; --backend " + std::string(backend_name(flavor)) + " takes the Ethernet ones:\n";
    std::size_t width = 0;
    for (const auto& device : devices)
    {
        width = std::max(width, as_typed(display_name(device)).size());
    }
    for (const auto& device : devices)
    {
        const std::string name = as_typed(display_name(device));
        text += "  --interface " + name + std::string(width - name.size() + 2, ' ') + state_text(device.flags);
        std::string more = device.description;
        if (device.name != display_name(device))
        {
            more += (more.empty() ? "" : "; ") + device.name;
        }
        if (!more.empty())
        {
            text += "  (" + more + ")";
        }
        text += "\n";
    }
    if (devices.empty())
    {
        text += flavor == PcapFlavor::Npcap
                    ? "  none. If Npcap was installed for administrators only, run this from a console started as administrator.\n"
                    : "  none\n";
    }
    return text;
}

std::unique_ptr<ExternalLink> make_pcap_link(std::shared_ptr<PcapLibrary> library, PcapLinkOptions options)
{
    return std::make_unique<PcapLink>(std::move(library), std::move(options));
}

} // namespace vehicle_sil::eth_link::detail

namespace vehicle_sil::eth_link {

std::unique_ptr<ExternalLink> make_pcap_link(PcapLinkOptions options)
{
    auto library = detail::load_pcap_library(options.library);
    return detail::make_pcap_link(std::move(library), std::move(options));
}

std::string pcap_interface_list(const std::string& library)
{
    const auto loaded = detail::load_pcap_library(library);
    return detail::format_pcap_devices(detail::pcap_devices(*loaded), loaded->flavor(), loaded->version());
}

} // namespace vehicle_sil::eth_link
