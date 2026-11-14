// SPDX-License-Identifier: MIT
//
// TEST ONLY. A fake pcap library: a shared library with the functions of Npcap's wpcap.dll and of
// libpcap that the pcap link uses, so that the tests go the way the real library goes (found and
// loaded at run time, its functions looked up by name, a device opened, frames sent and
// received, failures) on computers without Npcap or libpcap. It sends nothing anywhere: its
// devices live in this file.
//
// The devices (pcap_findalldevs lists them in this order):
//
//   fake0            answers every frame sent on it: to the frame's source, from 02:00:00:00:00:02,
//                    the bytes behind the two addresses and the EtherType inverted
//   fake1            sends and receives nothing by itself (the tests inject and read frames)
//   fake-echo        gives every frame sent on it back to the capture that sent it
//   fake-wifi        marked as wireless
//   fake-down        cannot be opened: the interface is down
//   fake-denied      cannot be opened: no permission
//   fake-nopromisc   opens with the warning that promiscuous mode is not supported
//   fake-raw         not Ethernet (link type 12)
//   fake-sendfail    sending fails
//   fake-sendfull    sending fails with the error number of a full queue
//   fake-fault       reading fails as soon as it is opened
//   fake-drops       counts 3 dropped frames for every frame sent
//
// Built a second time with FAKE_PCAP_OLD, it is a library from before pcap_set_immediate_mode.
//
// The tests reach the functions fake_pcap_* at the end of this file by name, like the others.
#include "pcap_api.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define FAKE_PCAP_EXPORT extern "C" __declspec(dllexport)
#else
#include <fcntl.h>
#include <unistd.h>
#define FAKE_PCAP_EXPORT extern "C" __attribute__((visibility("default")))
#endif

using vehicle_sil::eth_link::detail::PcapHandle;
using vehicle_sil::eth_link::detail::PcapInterface;
using vehicle_sil::eth_link::detail::PcapPacketHeader;
using vehicle_sil::eth_link::detail::PcapStatistics;

namespace {

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint32_t kUp = 0x02 | 0x04 | 0x10; // up, running, connected
constexpr std::uint32_t kWireless = 0x08;

/// "Packets are there", in the form the library hands out on this OS: an event on Windows, a
/// readable descriptor elsewhere.
class Signal
{
public:
#ifdef _WIN32
    Signal() : event_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~Signal() { CloseHandle(event_); }
    void raise() { SetEvent(event_); }
    void lower() { ResetEvent(event_); }
    [[nodiscard]] void* event() const { return event_; }

private:
    HANDLE event_;
#else
    Signal()
    {
        if (::pipe(pipe_) == 0)
        {
            for (const int end : pipe_)
            {
                ::fcntl(end, F_SETFD, FD_CLOEXEC);
                ::fcntl(end, F_SETFL, ::fcntl(end, F_GETFL) | O_NONBLOCK);
            }
        }
    }
    ~Signal()
    {
        ::close(pipe_[0]);
        ::close(pipe_[1]);
    }
    void raise()
    {
        if (!raised_)
        {
            raised_ = true;
            const char one = 1;
            (void)!::write(pipe_[1], &one, 1);
        }
    }
    void lower()
    {
        if (raised_)
        {
            raised_ = false;
            char one = 0;
            (void)!::read(pipe_[0], &one, 1);
        }
    }
    [[nodiscard]] int descriptor() const { return pipe_[0]; }

private:
    int pipe_[2] = {-1, -1};
    bool raised_ = false;
#endif
};

struct Device
{
    const char* name;
    const char* description;
    std::uint32_t flags = kUp;
    int activate_result = 0;
    const char* activate_text = "";
    int link_type = 1;
    bool answers = false;
    bool echoes = false;
    bool send_fails = false;
    bool send_full = false;
    bool read_fails = false;
    unsigned drops_per_send = 0;

    // What happened, for fake_pcap_get().
    unsigned dropped = 0;
    long closed = 0;
    long snaplen = -1, promisc = -1, timeout = -1, immediate = -1, nonblock = -1;
    std::vector<Bytes> sent;
    std::vector<PcapHandle*> open;
};

std::mutex g_mutex;

std::vector<Device>& devices()
{
    static std::vector<Device> list = [] {
        std::vector<Device> made;
        auto add = [&](const char* name, const char* description) -> Device& {
            Device device;
            device.name = name;
            device.description = description;
            made.push_back(device);
            return made.back();
        };
        add("fake0", "Fake Ethernet adapter with a device that answers").answers = true;
        add("fake1", "Fake Ethernet adapter");
        add("fake-echo", "Fake Ethernet adapter that returns what is sent").echoes = true;
        add("fake-wifi", "Fake wireless adapter").flags = kUp | kWireless;
        {
            Device& down = add("fake-down", "Fake Ethernet adapter that is down");
            down.flags = 0;
            down.activate_result = -9;
            down.activate_text = "fake-down: That device is not up";
        }
        {
            Device& denied = add("fake-denied", "Fake Ethernet adapter this user may not open");
            denied.activate_result = -8;
            denied.activate_text = "fake-denied: You don't have permission to capture on that device";
        }
        {
            Device& nopromisc = add("fake-nopromisc", "Fake Ethernet adapter without promiscuous mode");
            nopromisc.activate_result = 2;
            nopromisc.activate_text = "fake-nopromisc: promiscuous mode is not supported";
        }
        add("fake-raw", nullptr).link_type = 12;
        add("fake-sendfail", "Fake Ethernet adapter that cannot send").send_fails = true;
        add("fake-sendfull", "Fake Ethernet adapter whose queue is full").send_full = true;
        add("fake-fault", "Fake Ethernet adapter that cannot be read").read_fails = true;
        add("fake-drops", "Fake Ethernet adapter that drops frames").drops_per_send = 3;
        return made;
    }();
    return list;
}

Device* find_device(const char* name)
{
    for (auto& device : devices())
    {
        if (name != nullptr && std::strcmp(device.name, name) == 0)
        {
            return &device;
        }
    }
    return nullptr;
}

void copy_text(char* to, std::size_t size, const std::string& text)
{
    std::snprintf(to, size, "%s", text.c_str());
}

char* duplicate(const char* text)
{
    if (text == nullptr)
    {
        return nullptr;
    }
    char* const copy = static_cast<char*>(std::malloc(std::strlen(text) + 1));
    std::strcpy(copy, text);
    return copy;
}

} // namespace

struct vehicle_sil::eth_link::detail::PcapHandle
{
    Device* device = nullptr;
    bool active = false;
    int snaplen = 65535;
    int promisc = 0;
    int timeout = 0;
    int immediate = 0;
    int nonblock = 0;
    std::deque<Bytes> queue;
    Bytes current;
    PcapPacketHeader header{};
    char error[256] = {};
    Signal signal;
};

namespace {

/// A frame arrives at one capture.
void deliver(PcapHandle& handle, const Bytes& frame)
{
    handle.queue.push_back(frame);
    handle.signal.raise();
}

} // namespace

FAKE_PCAP_EXPORT const char* pcap_lib_version()
{
#ifdef FAKE_PCAP_OLD
    return "fake pcap version 0.9 (TEST ONLY)";
#else
    return "fake pcap version 1.0 (TEST ONLY)";
#endif
}

FAKE_PCAP_EXPORT int pcap_findalldevs(PcapInterface** list, char*)
{
    const std::lock_guard lock(g_mutex);
    PcapInterface* first = nullptr;
    PcapInterface** next = &first;
    for (const auto& device : devices())
    {
        auto* const entry = static_cast<PcapInterface*>(std::calloc(1, sizeof(PcapInterface)));
        entry->name = duplicate(device.name);
        entry->description = duplicate(device.description);
        entry->flags = device.flags;
        *next = entry;
        next = &entry->next;
    }
    *list = first;
    return 0;
}

FAKE_PCAP_EXPORT void pcap_freealldevs(PcapInterface* list)
{
    while (list != nullptr)
    {
        PcapInterface* const next = list->next;
        std::free(list->name);
        std::free(list->description);
        std::free(list);
        list = next;
    }
}

FAKE_PCAP_EXPORT PcapHandle* pcap_create(const char* source, char* error)
{
    const std::lock_guard lock(g_mutex);
    auto* const handle = new PcapHandle;
    // As the real library: a device that is not there is found out by pcap_activate().
    handle->device = find_device(source);
    (void)error;
    return handle;
}

FAKE_PCAP_EXPORT int pcap_set_snaplen(PcapHandle* handle, int bytes)
{
    handle->snaplen = bytes;
    return 0;
}

FAKE_PCAP_EXPORT int pcap_set_promisc(PcapHandle* handle, int on)
{
    handle->promisc = on;
    return 0;
}

FAKE_PCAP_EXPORT int pcap_set_timeout(PcapHandle* handle, int milliseconds)
{
    handle->timeout = milliseconds;
    return 0;
}

#ifndef FAKE_PCAP_OLD
FAKE_PCAP_EXPORT int pcap_set_immediate_mode(PcapHandle* handle, int on)
{
    handle->immediate = on;
    return 0;
}
#endif

FAKE_PCAP_EXPORT int pcap_activate(PcapHandle* handle)
{
    const std::lock_guard lock(g_mutex);
    Device* const device = handle->device;
    if (device == nullptr)
    {
        copy_text(handle->error, sizeof(handle->error), "No such device exists");
        return -5;
    }
    copy_text(handle->error, sizeof(handle->error), device->activate_text);
    if (device->activate_result < 0)
    {
        return device->activate_result;
    }
    handle->active = true;
    device->open.push_back(handle);
    device->snaplen = handle->snaplen;
    device->promisc = handle->promisc;
    device->timeout = handle->timeout;
    device->immediate = handle->immediate;
    device->nonblock = handle->nonblock;
    if (device->read_fails)
    {
        handle->signal.raise();
    }
    return device->activate_result == 2 && handle->promisc == 0 ? 0 : device->activate_result;
}

FAKE_PCAP_EXPORT int pcap_datalink(PcapHandle* handle)
{
    return handle->device != nullptr ? handle->device->link_type : -1;
}

FAKE_PCAP_EXPORT int pcap_setnonblock(PcapHandle* handle, int on, char*)
{
    const std::lock_guard lock(g_mutex);
    handle->nonblock = on;
    if (handle->device != nullptr)
    {
        handle->device->nonblock = on;
    }
    return 0;
}

FAKE_PCAP_EXPORT void pcap_close(PcapHandle* handle)
{
    {
        const std::lock_guard lock(g_mutex);
        if (handle->device != nullptr && handle->active)
        {
            std::erase(handle->device->open, handle);
            ++handle->device->closed;
        }
    }
    delete handle;
}

FAKE_PCAP_EXPORT int pcap_next_ex(PcapHandle* handle, PcapPacketHeader** header, const unsigned char** bytes)
{
    const std::lock_guard lock(g_mutex);
    if (!handle->active)
    {
        copy_text(handle->error, sizeof(handle->error), "the capture is not activated");
        return -1;
    }
    if (handle->device->read_fails)
    {
        copy_text(handle->error, sizeof(handle->error), "The interface disappeared (fake)");
        return -1;
    }
    if (handle->queue.empty())
    {
        handle->signal.lower();
        return 0;
    }
    handle->current = std::move(handle->queue.front());
    handle->queue.pop_front();
    if (handle->queue.empty())
    {
        handle->signal.lower();
    }
    handle->header = {};
    handle->header.wire_bytes = static_cast<std::uint32_t>(handle->current.size());
    handle->current.resize(std::min(handle->current.size(), static_cast<std::size_t>(handle->snaplen)));
    handle->header.captured_bytes = static_cast<std::uint32_t>(handle->current.size());
    *header = &handle->header;
    *bytes = handle->current.data();
    return 1;
}

FAKE_PCAP_EXPORT int pcap_sendpacket(PcapHandle* handle, const unsigned char* bytes, int size)
{
    const std::lock_guard lock(g_mutex);
    if (!handle->active)
    {
        copy_text(handle->error, sizeof(handle->error), "the capture is not activated");
        return -1;
    }
    Device& device = *handle->device;
    if (device.send_full)
    {
        copy_text(handle->error, sizeof(handle->error), "send: No buffer space available (fake)");
        errno = ENOBUFS;
        return -1;
    }
    if (device.send_fails)
    {
        copy_text(handle->error, sizeof(handle->error), "send: Network is down (fake)");
        errno = 0;
        return -1;
    }
    const Bytes frame(bytes, bytes + size);
    device.sent.push_back(frame);
    device.dropped += device.drops_per_send;
    if (device.echoes)
    {
        deliver(*handle, frame);
    }
    if (device.answers && frame.size() >= 14)
    {
        Bytes answer = frame;
        const std::uint8_t deviceMac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x02};
        for (std::size_t i = 0; i < 6; ++i)
        {
            answer[i] = frame[6 + i];
            answer[6 + i] = deviceMac[i];
        }
        for (std::size_t i = 14; i < answer.size(); ++i)
        {
            answer[i] = static_cast<std::uint8_t>(~answer[i]);
        }
        for (PcapHandle* const open : device.open)
        {
            deliver(*open, answer);
        }
    }
    return 0;
}

FAKE_PCAP_EXPORT char* pcap_geterr(PcapHandle* handle)
{
    return handle->error;
}

FAKE_PCAP_EXPORT int pcap_stats(PcapHandle* handle, PcapStatistics* statistics)
{
    const std::lock_guard lock(g_mutex);
    if (!handle->active)
    {
        return -1;
    }
    statistics->received = 0;
    statistics->dropped = handle->device->dropped;
    statistics->dropped_by_interface = 1000; // the link must not count these
    return 0;
}

#ifdef _WIN32
FAKE_PCAP_EXPORT void* pcap_getevent(PcapHandle* handle)
{
    return handle->signal.event();
}
#else
FAKE_PCAP_EXPORT int pcap_get_selectable_fd(PcapHandle* handle)
{
    return handle->signal.descriptor();
}
#endif

// ---- The tests' controls ----

/// A frame arrives at every open capture of `device`. Returns how many captures got it, -1 if
/// there is no such device.
FAKE_PCAP_EXPORT int fake_pcap_inject(const char* device, const unsigned char* bytes, int size)
{
    const std::lock_guard lock(g_mutex);
    Device* const found = find_device(device);
    if (found == nullptr)
    {
        return -1;
    }
    for (PcapHandle* const open : found->open)
    {
        deliver(*open, Bytes(bytes, bytes + size));
    }
    return static_cast<int>(found->open.size());
}

/// Copies the frame number `index` (from 0) that was sent on `device` to `out` and returns its
/// size; -1 if there is no such frame or it does not fit.
FAKE_PCAP_EXPORT int fake_pcap_sent(const char* device, int index, unsigned char* out, int capacity)
{
    const std::lock_guard lock(g_mutex);
    const Device* const found = find_device(device);
    if (found == nullptr || index < 0 || static_cast<std::size_t>(index) >= found->sent.size())
    {
        return -1;
    }
    const Bytes& frame = found->sent[static_cast<std::size_t>(index)];
    if (frame.size() > static_cast<std::size_t>(capacity))
    {
        return -1;
    }
    std::copy(frame.begin(), frame.end(), out);
    return static_cast<int>(frame.size());
}

/// "sent" (frames sent), "open" (open captures), "closed" (captures closed), and what the last
/// capture that was opened asked for: "snaplen", "promisc", "timeout", "immediate", "nonblock"
/// (-1: never asked). -2 for anything else.
FAKE_PCAP_EXPORT long fake_pcap_get(const char* device, const char* what)
{
    const std::lock_guard lock(g_mutex);
    const Device* const found = find_device(device);
    if (found == nullptr || what == nullptr)
    {
        return -2;
    }
    const std::string key = what;
    if (key == "sent") return static_cast<long>(found->sent.size());
    if (key == "open") return static_cast<long>(found->open.size());
    if (key == "closed") return found->closed;
    if (key == "snaplen") return found->snaplen;
    if (key == "promisc") return found->promisc;
    if (key == "timeout") return found->timeout;
    if (key == "immediate") return found->immediate;
    if (key == "nonblock") return found->nonblock;
    return -2;
}

/// "read_fails" (1: reading fails from now on), "drops" (adds to the count of dropped frames).
/// Returns 0, or -2 for anything else.
FAKE_PCAP_EXPORT int fake_pcap_set(const char* device, const char* what, long value)
{
    const std::lock_guard lock(g_mutex);
    Device* const found = find_device(device);
    if (found == nullptr || what == nullptr)
    {
        return -2;
    }
    const std::string key = what;
    if (key == "read_fails")
    {
        found->read_fails = value != 0;
        for (PcapHandle* const open : found->open)
        {
            open->signal.raise();
        }
        return 0;
    }
    if (key == "drops")
    {
        found->dropped += static_cast<unsigned>(value);
        return 0;
    }
    return -2;
}
