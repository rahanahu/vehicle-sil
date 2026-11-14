// SPDX-License-Identifier: MIT
//
// The pcap link on the fake pcap library (fake_pcap/fake_pcap.cpp), which is loaded the way the
// real one is: found by its path, its functions looked up by name. What the link asks of a
// capture, frames both ways, what stop() does, and each failure: a library that is not there or
// too old, a device that cannot be opened, sending and reading that fail, dropped frames.
//
// The link opens a capture in two ways: Npcap's by pcap_open(), so that the capture does not get
// what it sent itself, and libpcap's by pcap_create(). The tests of what differs between them go
// both ways on every platform (load_pcap_library_as()); the others go the way of this platform.
#include "check_text.hpp"
#include "fake_link.hpp"
#include "test_harness.hpp"

#include "pcap_link.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil::eth_link;
using namespace vehicle_sil::eth_link::detail;
using vehicle_sil::fake_eth::Bytes;
using vehicle_sil::fake_eth::make_frame;
using vehicle_sil::unit::eventually;

#ifdef _WIN32
const char* const kSuffix = ".dll";
#else
const char* const kSuffix = ".so";
#endif
const std::string kFake = std::string(VEHICLE_SIL_FAKE_PCAP_DIR) + "/vehicle_sil_fake_pcap" + kSuffix;
const std::string kFakeOld = std::string(VEHICLE_SIL_FAKE_PCAP_DIR) + "/vehicle_sil_fake_pcap_old" + kSuffix;
const std::string kFakeNoOpen = std::string(VEHICLE_SIL_FAKE_PCAP_DIR) + "/vehicle_sil_fake_pcap_no_open" + kSuffix;

/// What pcap_open() takes as its flags.
constexpr long kPromiscuous = 1;
constexpr long kNoCaptureLocal = 8;
constexpr long kMaxResponsiveness = 16;

const char* const kEcu = "02:00:00:00:00:01";
const char* const kOutside = "02:00:00:00:00:99";

/// The fake library, loaded as the link loads it, and its controls.
struct Fake
{
    std::shared_ptr<PcapLibrary> library;

    Fake() : library(load_pcap_library(kFake)) {}
    /// The same library taken for `flavor`: the link opens its captures that library's way.
    explicit Fake(PcapFlavor flavor) : library(load_pcap_library_as(kFake, flavor)) {}

    template <typename Function>
    Function* control(const char* name) const
    {
        void* const address = library->symbol(name);
        if (address == nullptr)
        {
            throw std::runtime_error(std::string("the fake pcap library lacks its control ") + name);
        }
        return reinterpret_cast<Function*>(address);
    }

    long get(const char* device, const char* what) const { return control<long(const char*, const char*)>("fake_pcap_get")(device, what); }
    void set(const char* device, const char* what, long value) const
    {
        CHECK_EQ(control<int(const char*, const char*, long)>("fake_pcap_set")(device, what, value), 0);
    }
    int inject(const char* device, const Bytes& frame) const
    {
        return control<int(const char*, const unsigned char*, int)>("fake_pcap_inject")(device, frame.data(), static_cast<int>(frame.size()));
    }
    Bytes sent(const char* device, long index) const
    {
        Bytes frame(70000);
        const int size = control<int(const char*, int, unsigned char*, int)>("fake_pcap_sent")(device, static_cast<int>(index), frame.data(),
                                                                                              static_cast<int>(frame.size()));
        frame.resize(size < 0 ? 0 : static_cast<std::size_t>(size));
        return frame;
    }
    std::unique_ptr<ExternalLink> link(const std::string& name, bool promiscuous = false) const
    {
        return make_pcap_link(library, {name, promiscuous, kFake});
    }
};

/// What a handler received.
struct Recorder
{
    mutable std::mutex mutex;
    std::vector<Bytes> frames;
    std::vector<std::string> warnings;
    std::vector<std::string> faults;
    std::uint64_t overruns = 0;
    bool incomplete = false;
    bool from_this_thread = false;
    const std::thread::id maker = std::this_thread::get_id();

    ExternalLink::EventHandler handler()
    {
        return [this](const LinkEvent& event) {
            const std::lock_guard lock(mutex);
            from_this_thread = from_this_thread || std::this_thread::get_id() == maker;
            switch (event.kind)
            {
            case LinkEventKind::Frame:
                frames.emplace_back(event.frame.begin(), event.frame.end());
                incomplete = incomplete || event.checksum_incomplete;
                break;
            case LinkEventKind::Overrun: overruns += event.count; break;
            case LinkEventKind::Warning: warnings.emplace_back(event.detail); break;
            case LinkEventKind::Fault: faults.emplace_back(event.detail); break;
            case LinkEventKind::LinkState: break;
            }
        };
    }
    std::size_t frame_count() const
    {
        const std::lock_guard lock(mutex);
        return frames.size();
    }
    std::size_t fault_count() const
    {
        const std::lock_guard lock(mutex);
        return faults.size();
    }
    std::size_t warning_count() const
    {
        const std::lock_guard lock(mutex);
        return warnings.size();
    }
};

template <typename Function>
std::string error_of(Function&& function, LinkErrorKind kind)
{
    try
    {
        function();
    }
    catch (const LinkError& e)
    {
        return e.kind() == kind ? e.what() : std::string("wrong kind (") + std::string(to_string(e.kind())) + "): " + e.what();
    }
    return "no LinkError";
}

/// The message of start() on the device `name`, which must fail with `kind`.
std::string start_error(const Fake& fake, const std::string& name, LinkErrorKind kind, bool promiscuous = false)
{
    const auto link = fake.link(name, promiscuous);
    Recorder recorder;
    return error_of([&] { link->start(recorder.handler()); }, kind);
}

} // namespace

TEST_CASE(pcap_library_is_loaded_by_its_path_and_lists_its_devices)
{
    const Fake fake;
    CHECK_TEXT(fake.library->path().substr(fake.library->path().size() - std::string("vehicle_sil_fake_pcap").size() - std::string(kSuffix).size()),
               std::string("vehicle_sil_fake_pcap") + kSuffix);
    CHECK_TEXT(fake.library->version(), "fake pcap version 1.0 (TEST ONLY)");
    const auto devices = pcap_devices(*fake.library);
    CHECK_EQ(devices.size(), std::size_t{13});
    CHECK_TEXT(devices.at(0).name, "fake0");
    CHECK_TEXT(devices.at(0).description, "Fake Ethernet adapter with a device that answers");
    CHECK_EQ(devices.at(0).flags, kPcapIfUp | kPcapIfRunning | kPcapIfConnected);
    CHECK_TEXT(devices.at(7).name, "fake-raw");
    CHECK_TEXT(devices.at(7).description, ""); // the library gave no description
    const std::string list = pcap_interface_list(kFake);
    const std::string head = std::string("interfaces ") + std::string(library_name(fake.library->flavor())) +
                             " offers (fake pcap version 1.0 (TEST ONLY)); --backend " +
                             std::string(backend_name(fake.library->flavor())) +
                             " takes the Ethernet ones:\n"
                             "  --interface fake0           up, connected  (Fake Ethernet adapter with a device that answers)\n"
                             "  --interface fake1           up, connected  (Fake Ethernet adapter)\n";
    CHECK_TEXT(list.substr(0, head.size()), head);
    CHECK(list.find("  --interface fake-wifi       up, connected, wireless  (Fake wireless adapter)\n") != std::string::npos);
    CHECK(list.find("  --interface fake-down       down  (Fake Ethernet adapter that is down)\n") != std::string::npos);
}

TEST_CASE(pcap_library_that_is_not_there_is_library_missing)
{
    const std::string nowhere = std::string(VEHICLE_SIL_FAKE_PCAP_DIR) + "/no_such_pcap" + kSuffix;
    const std::string message = error_of([&] { (void)load_pcap_library(nowhere); }, LinkErrorKind::LibraryMissing);
#ifdef _WIN32
    const std::string start = "Npcap is not installed: cannot load ";
    const std::string end = "Npcap is not part of vehicle-sil: install it yourself from https://npcap.com (see its licence for the "
                            "limits of the free edition), then run --list-interfaces.";
#else
    const std::string start = "libpcap is not installed: cannot load " + nowhere + " (";
    const std::string end = "or use --backend packet, which needs no library.";
#endif
    CHECK_TEXT(message.substr(0, start.size()), start);
    CHECK(message.find("no_such_pcap") != std::string::npos);
    CHECK_TEXT(message.substr(message.size() - end.size()), end);
    CHECK_TEXT(error_of([&] { (void)make_pcap_link(PcapLinkOptions{"fake0", false, nowhere}); }, LinkErrorKind::LibraryMissing), message);
    CHECK_TEXT(error_of([&] { (void)pcap_interface_list(nowhere); }, LinkErrorKind::LibraryMissing), message);
}

TEST_CASE(pcap_library_from_before_a_function_the_link_needs_is_refused_with_its_version)
{
    const std::string message = error_of([&] { (void)load_pcap_library(kFakeOld); }, LinkErrorKind::LibraryMissing);
    const std::string wanted = "(fake pcap version 0.9 (TEST ONLY)) lacks the function pcap_set_immediate_mode, which the bridge needs: "
                               "it is too old, or it is not ";
    CHECK(message.find("vehicle_sil_fake_pcap_old") != std::string::npos);
    CHECK(message.find(wanted) != std::string::npos);
    if (message.find(wanted) == std::string::npos)
    {
        CHECK_TEXT(message, wanted);
    }
}

TEST_CASE(pcap_library_of_this_platform_is_taken_for_npcap_on_windows_and_for_libpcap_elsewhere)
{
    const Fake fake;
#ifdef _WIN32
    CHECK(fake.library->flavor() == PcapFlavor::Npcap);
#else
    CHECK(fake.library->flavor() == PcapFlavor::Libpcap);
#endif
    CHECK(Fake(PcapFlavor::Npcap).library->flavor() == PcapFlavor::Npcap);
    CHECK(Fake(PcapFlavor::Libpcap).library->flavor() == PcapFlavor::Libpcap);
}

TEST_CASE(pcap_library_without_pcap_open_is_refused_as_npcap_and_taken_as_libpcap)
{
    const std::string message = error_of([&] { (void)load_pcap_library_as(kFakeNoOpen, PcapFlavor::Npcap); }, LinkErrorKind::LibraryMissing);
    const std::string wanted = "(fake pcap version 1.0 (TEST ONLY)) lacks the function pcap_open, which the bridge needs: it is too "
                               "old, or it is not Npcap. Install a current one. Npcap is not part of vehicle-sil: install it "
                               "yourself from https://npcap.com (see its licence for the limits of the free edition), then run "
                               "--list-interfaces.";
    CHECK(message.find("vehicle_sil_fake_pcap_no_open") != std::string::npos);
    CHECK(message.size() >= wanted.size());
    if (message.size() >= wanted.size())
    {
        CHECK_TEXT(message.substr(message.size() - wanted.size()), wanted);
    }
    // libpcap is not asked for it: its captures are opened by pcap_create().
    const auto library = load_pcap_library_as(kFakeNoOpen, PcapFlavor::Libpcap);
    const auto link = make_pcap_link(library, {"fake1", false, kFakeNoOpen});
    Recorder recorder;
    link->start(recorder.handler());
    link->stop();
}

TEST_CASE(pcap_link_opens_the_capture_for_frames_at_once_without_blocking)
{
    for (const PcapFlavor flavor : {PcapFlavor::Libpcap, PcapFlavor::Npcap})
    {
        const Fake fake(flavor);
        const bool npcap = flavor == PcapFlavor::Npcap;
        const auto link = fake.link("fake1", true);
        CHECK_TEXT(link->describe(), std::string(backend_name(flavor)) + " 'fake1'");
        Recorder recorder;
        link->start(recorder.handler());
        CHECK_EQ(fake.get("fake1", "open"), 1L);
        CHECK_EQ(fake.get("fake1", "immediate"), 1L);
        CHECK_EQ(fake.get("fake1", "nonblock"), 1L);
        CHECK_EQ(fake.get("fake1", "promisc"), 1L);
        CHECK_EQ(fake.get("fake1", "snaplen"), 65536L);
        CHECK(fake.get("fake1", "timeout") > 0);
        // Npcap: by pcap_open(), with the flag that keeps the capture's own transmissions away.
        // libpcap: by pcap_create(); its captures do not get them anyway.
        CHECK_EQ(fake.get("fake1", "open_flags"), npcap ? kPromiscuous | kNoCaptureLocal | kMaxResponsiveness : -1L);
        CHECK_TEXT(link->describe(), npcap ? "npcap 'fake1' (network adapter through Npcap)" : "pcap 'fake1' (network interface through libpcap)");
        CHECK(!link->echoes_own_transmissions());
        link->start(recorder.handler()); // started already: nothing is opened again
        CHECK_EQ(fake.get("fake1", "open"), 1L);
        link->stop();
        CHECK_EQ(fake.get("fake1", "open"), 0L);

        const auto quiet = fake.link("fake1");
        quiet->start(recorder.handler());
        CHECK_EQ(fake.get("fake1", "promisc"), 0L);
        CHECK_EQ(fake.get("fake1", "immediate"), 1L);
        CHECK_EQ(fake.get("fake1", "open_flags"), npcap ? kNoCaptureLocal | kMaxResponsiveness : -1L);
        quiet->stop();
        CHECK_EQ(recorder.warning_count(), std::size_t{0});
    }
}

TEST_CASE(pcap_link_on_npcap_does_not_get_what_it_sent_and_gets_what_other_captures_send)
{
    // fake-echo returns what is sent on it to the capture that sent it, as an adapter of Npcap
    // does, unless the capture asked it not to. The link says it does not get its own
    // transmissions (echoes_own_transmissions() is false), and the bridge relies on that.
    const Fake npcap(PcapFlavor::Npcap);
    const auto a = npcap.link("fake-echo");
    const auto b = npcap.link("fake-echo");
    Recorder atA;
    Recorder atB;
    a->start(atA.handler());
    b->start(atB.handler());
    const Bytes fromA = make_frame(kOutside, kEcu, 0x88B5, 46, 0xA1);
    const Bytes fromB = make_frame(kEcu, kOutside, 0x88B5, 46, 0xB2);
    CHECK(a->transmit(fromA) == TransmitResult::Ok);
    CHECK(eventually([&] { return atB.frame_count() == 1; }, 10000ms)); // the other capture gets it
    CHECK(b->transmit(fromB) == TransmitResult::Ok);
    CHECK(eventually([&] { return atA.frame_count() == 1; }, 10000ms));
    // A frame from outside behind them: what a capture has by then is everything it was given.
    const Bytes last = make_frame(kEcu, kOutside, 0x88B5, 46, 0xC3);
    CHECK_EQ(npcap.inject("fake-echo", last), 2);
    CHECK(eventually([&] { return atA.frame_count() == 2 && atB.frame_count() == 2; }, 10000ms));
    a->stop();
    b->stop();
    {
        const std::scoped_lock lock(atA.mutex, atB.mutex);
        CHECK_EQ(atA.frames.size(), std::size_t{2});
        CHECK_EQ(atB.frames.size(), std::size_t{2});
        CHECK(atA.frames.at(0) == fromB); // not fromA, its own, before it
        CHECK(atB.frames.at(0) == fromA);
        CHECK(atA.frames.at(1) == last);
        CHECK(atB.frames.at(1) == last);
    }

    // The same device does return them to a capture that did not ask (libpcap's way of opening
    // has no such flag): the fake is not just a device that returns nothing.
    const Fake libpcap(PcapFlavor::Libpcap);
    const auto plain = libpcap.link("fake-echo");
    Recorder atPlain;
    plain->start(atPlain.handler());
    CHECK(plain->transmit(fromA) == TransmitResult::Ok);
    CHECK(eventually([&] { return atPlain.frame_count() == 1; }, 10000ms));
    plain->stop();
    const std::lock_guard lock(atPlain.mutex);
    CHECK(atPlain.frames.at(0) == fromA);
}

TEST_CASE(pcap_link_delivers_received_frames_unchanged_in_order_from_its_own_thread)
{
    const Fake fake;
    const auto link = fake.link("fake1");
    Recorder recorder;
    link->start(recorder.handler());
    // More than the link reads in one go.
    std::vector<Bytes> frames;
    for (std::uint8_t i = 0; i < 200; ++i)
    {
        frames.push_back(make_frame(kEcu, kOutside, 0x88B5, 46 + i, i));
        CHECK_EQ(fake.inject("fake1", frames.back()), 1);
    }
    CHECK(eventually([&] { return recorder.frame_count() == frames.size(); }, 10000ms));
    // A frame longer than the longest the bridge may pass arrives cut, and still too long.
    CHECK_EQ(fake.inject("fake1", Bytes(70000, 0x5A)), 1);
    CHECK(eventually([&] { return recorder.frame_count() == frames.size() + 1; }, 10000ms));
    link->stop();
    const std::lock_guard lock(recorder.mutex);
    for (std::size_t i = 0; i < frames.size(); ++i)
    {
        CHECK(recorder.frames[i] == frames[i]);
    }
    CHECK_EQ(recorder.frames.back().size(), std::size_t{65536});
    CHECK(!recorder.from_this_thread);
    CHECK(!recorder.incomplete);
    CHECK_EQ(recorder.overruns, std::uint64_t{0});
}

TEST_CASE(pcap_link_sends_frames_unchanged_and_gets_the_answers)
{
    const Fake fake;
    const auto link = fake.link("fake0");
    Recorder recorder;
    link->start(recorder.handler());
    const long before = fake.get("fake0", "sent");
    const Bytes frame = make_frame(kOutside, kEcu, 0x88B5, 100, 7);
    CHECK(link->transmit(frame) == TransmitResult::Ok);
    CHECK_EQ(fake.get("fake0", "sent"), before + 1);
    CHECK(fake.sent("fake0", before) == frame);
    CHECK(eventually([&] { return recorder.frame_count() == 1; }, 10000ms));
    link->stop();
    CHECK_TEXT(link->last_error(), "");
    const std::lock_guard lock(recorder.mutex);
    // The fake device answers to the frame's source from its own address.
    CHECK(Bytes(recorder.frames[0].begin(), recorder.frames[0].begin() + 12) ==
          Bytes({0x02, 0, 0, 0, 0, 0x01, 0x02, 0, 0, 0, 0, 0x02}));
    CHECK_EQ(recorder.frames[0].size(), frame.size());
    CHECK_EQ(recorder.frames[0][14], static_cast<std::uint8_t>(~frame[14]));
}

TEST_CASE(pcap_link_stop_ends_the_events_and_closes_the_capture_and_it_starts_again)
{
    const Fake fake;
    const auto link = fake.link("fake0");
    link->stop(); // not started: nothing happens
    Recorder recorder;
    const long closed = fake.get("fake0", "closed");
    link->start(recorder.handler());
    const Bytes frame = make_frame(kOutside, kEcu, 0x88B5);
    CHECK(link->transmit(frame) == TransmitResult::Ok);
    CHECK(eventually([&] { return recorder.frame_count() == 1; }, 10000ms)); // this device answers what is sent
    link->stop();
    CHECK_EQ(fake.get("fake0", "open"), 0L);
    CHECK_EQ(fake.get("fake0", "closed"), closed + 1);
    CHECK(link->transmit(frame) == TransmitResult::Failed);
    CHECK_TEXT(link->last_error(), "the link on 'fake0' is not started");
    link->stop();

    link->start(recorder.handler());
    CHECK_TEXT(link->last_error(), "");
    CHECK(link->transmit(frame) == TransmitResult::Ok);
    CHECK(eventually([&] { return recorder.frame_count() == 2; }, 10000ms));
    // Destroying a started link stops it.
}

TEST_CASE(pcap_link_takes_frames_from_many_threads_while_it_receives)
{
    const Fake fake;
    const auto link = fake.link("fake0");
    Recorder recorder;
    link->start(recorder.handler());
    const long before = fake.get("fake0", "sent");
    constexpr int kThreads = 4;
    constexpr int kFrames = 100;
    std::vector<std::thread> threads;
    std::atomic<int> failed{0};
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kFrames; ++i)
            {
                if (link->transmit(make_frame(kOutside, kEcu, 0x88B5, 46, static_cast<std::uint8_t>(t))) != TransmitResult::Ok)
                {
                    ++failed;
                }
            }
        });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }
    CHECK_EQ(failed.load(), 0);
    CHECK_EQ(fake.get("fake0", "sent"), before + kThreads * kFrames);
    CHECK(eventually([&] { return recorder.frame_count() == static_cast<std::size_t>(kThreads * kFrames); }, 10000ms));
    link->stop();
}

TEST_CASE(pcap_link_tells_a_full_queue_from_a_failed_transmission)
{
    const Fake fake;
    Recorder recorder;
    const Bytes frame = make_frame(kOutside, kEcu, 0x88B5);
    const auto failing = fake.link("fake-sendfail");
    failing->start(recorder.handler());
    CHECK(failing->transmit(frame) == TransmitResult::Failed);
    CHECK_TEXT(failing->last_error(), "sending on 'fake-sendfail' failed: send: Network is down (fake)");
    failing->stop();
    const auto full = fake.link("fake-sendfull");
    full->start(recorder.handler());
    CHECK(full->transmit(frame) == TransmitResult::QueueFull);
    CHECK_TEXT(full->last_error(), "");
    full->stop();
    CHECK_EQ(recorder.fault_count(), std::size_t{0});
}

TEST_CASE(pcap_link_reports_a_fault_when_reading_fails_and_sends_no_more)
{
    const Fake fake;
    const auto link = fake.link("fake1");
    Recorder recorder;
    link->start(recorder.handler());
    const Bytes frame = make_frame(kEcu, kOutside, 0x88B5);
    CHECK_EQ(fake.inject("fake1", frame), 1);
    CHECK(eventually([&] { return recorder.frame_count() == 1; }, 10000ms));
    fake.set("fake1", "read_fails", 1);
    CHECK(eventually([&] { return recorder.fault_count() == 1; }, 10000ms));
    CHECK_TEXT(link->last_error(), "reading from 'fake1' failed: The interface disappeared (fake)");
    CHECK(link->transmit(frame) == TransmitResult::Failed);
    CHECK_TEXT(link->last_error(), "reading from 'fake1' failed: The interface disappeared (fake)");
    link->stop();
    fake.set("fake1", "read_fails", 0);
    const std::lock_guard lock(recorder.mutex);
    CHECK_TEXT(recorder.faults.at(0), "reading from 'fake1' failed: The interface disappeared (fake)");
    CHECK_EQ(recorder.faults.size(), std::size_t{1}); // once, and no frame after it
    CHECK_EQ(recorder.frames.size(), std::size_t{1});

    // A device that cannot be read from the start: the fault comes from the receive thread too.
    const auto faulty = fake.link("fake-fault");
    Recorder second;
    faulty->start(second.handler());
    CHECK(eventually([&] { return second.fault_count() == 1; }, 10000ms));
    faulty->stop();
}

TEST_CASE(pcap_link_reports_the_frames_the_library_dropped_once_each)
{
    const Fake fake;
    const auto link = fake.link("fake-drops");
    Recorder recorder;
    link->start(recorder.handler());
    const Bytes frame = make_frame(kOutside, kEcu, 0x88B5);
    CHECK(link->transmit(frame) == TransmitResult::Ok); // the fake counts 3 drops for each
    CHECK(link->transmit(frame) == TransmitResult::Ok);
    // Reported while it runs (the thread looks a few times a second) ...
    CHECK(eventually(
        [&] {
            const std::lock_guard lock(recorder.mutex);
            return recorder.overruns == 6;
        },
        10000ms));
    CHECK(link->transmit(frame) == TransmitResult::Ok);
    // ... and what is left when it stops, by stop() itself.
    link->stop();
    const std::lock_guard lock(recorder.mutex);
    CHECK_EQ(recorder.overruns, std::uint64_t{9});
}

TEST_CASE(pcap_link_refuses_devices_it_cannot_open_and_says_why)
{
    for (const PcapFlavor flavor : {PcapFlavor::Libpcap, PcapFlavor::Npcap})
    {
        const Fake fake(flavor);
        const std::string executable = fake.library->executable_path();
        const long rawClosed = fake.get("fake-raw", "closed");
        if (flavor == PcapFlavor::Npcap)
        {
            // pcap_open() tells a failure as a text only: the message passes it on, and says what to look at.
            CHECK_TEXT(start_error(fake, "fake-down", LinkErrorKind::Other),
                       "cannot open 'fake-down' with Npcap (pcap_open: fake-down: That device is not up). Run --list-interfaces "
                       "--backend npcap to see whether Npcap still offers the adapter; if Npcap was installed for administrators "
                       "only, run the bridge from a console started as administrator.");
            CHECK_TEXT(start_error(fake, "fake-denied", LinkErrorKind::Other),
                       pcap_open_refused("fake-denied", "fake-denied: You don't have permission to capture on that device").what());
        }
        else
        {
            CHECK_TEXT(start_error(fake, "fake-down", LinkErrorKind::Other),
                       pcap_open_failed(flavor, "fake-down", "pcap_activate", -9, "fake-down: That device is not up", executable).what());
            CHECK_TEXT(start_error(fake, "fake-denied", LinkErrorKind::PermissionDenied),
                       pcap_open_failed(flavor, "fake-denied", "pcap_activate", -8,
                                        "fake-denied: You don't have permission to capture on that device", executable)
                           .what());
        }
        CHECK_TEXT(start_error(fake, "fake-raw", LinkErrorKind::NotEthernet), pcap_not_ethernet(flavor, "fake-raw", 12).what());
        const std::string none = start_error(fake, "eth9", LinkErrorKind::NoSuchInterface);
        const std::string start = "no interface 'eth9' for --backend " + std::string(backend_name(flavor)) + ". Interfaces " +
                                  std::string(library_name(flavor)) + " offers here: fake0, fake1, fake-echo, ";
        CHECK_TEXT(none.substr(0, start.size()), start);
        // Nothing stays open after a start that failed, and the link can be started on nothing.
        for (const char* device : {"fake-down", "fake-denied", "fake-raw"})
        {
            CHECK_EQ(fake.get(device, "open"), 0L);
        }
        CHECK_EQ(fake.get("fake-raw", "closed"), rawClosed + 1);
    }
}

TEST_CASE(pcap_link_warns_about_a_wireless_adapter_and_about_no_promiscuous_mode)
{
    const Fake fake;
    {
        const auto link = fake.link("fake-wifi");
        Recorder recorder;
        link->start(recorder.handler());
        CHECK(eventually([&] { return recorder.warning_count() == 1; }, 10000ms));
        link->stop();
        CHECK_TEXT(recorder.warnings.at(0), pcap_wireless_warning("fake-wifi"));
    }
    {
        const Fake libpcap(PcapFlavor::Libpcap);
        const auto link = libpcap.link("fake-nopromisc", true);
        Recorder recorder;
        link->start(recorder.handler());
        CHECK(eventually([&] { return recorder.warning_count() == 1; }, 10000ms));
        link->stop();
        CHECK_TEXT(recorder.warnings.at(0),
                   pcap_activate_warning("fake-nopromisc", 2, "fake-nopromisc: promiscuous mode is not supported"));
    }
    {
        // Npcap's pcap_open() does not tell a warning of the activation: the capture opens, without one.
        const Fake npcap(PcapFlavor::Npcap);
        const auto link = npcap.link("fake-nopromisc", true);
        Recorder recorder;
        link->start(recorder.handler());
        CHECK_EQ(npcap.inject("fake-nopromisc", make_frame(kEcu, kOutside, 0x88B5)), 1);
        CHECK(eventually([&] { return recorder.frame_count() == 1; }, 10000ms));
        link->stop();
        CHECK_EQ(recorder.warning_count(), std::size_t{0});
    }
    {
        const auto link = fake.link("fake-nopromisc"); // not asked for: nothing to warn about
        Recorder recorder;
        link->start(recorder.handler());
        CHECK_EQ(fake.inject("fake-nopromisc", make_frame(kEcu, kOutside, 0x88B5)), 1);
        CHECK(eventually([&] { return recorder.frame_count() == 1; }, 10000ms));
        link->stop();
        CHECK_EQ(recorder.warning_count(), std::size_t{0});
    }
}

TEST_CASE(pcap_link_receives_from_a_library_that_never_tells_that_frames_are_there)
{
    // A library whose event is never set (Windows) or whose descriptor never gets readable: the
    // link looks after every wait that ran out, so the frames arrive late and not never, and it
    // says so once.
    const Fake fake;
    const auto link = fake.link("fake-quiet");
    Recorder recorder;
    link->start(recorder.handler());
    std::vector<Bytes> frames;
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        frames.push_back(make_frame(kEcu, kOutside, 0x88B5, 46, i));
        CHECK_EQ(fake.inject("fake-quiet", frames.back()), 1);
        CHECK(eventually([&] { return recorder.frame_count() == frames.size(); }, 10000ms));
    }
    CHECK(eventually([&] { return recorder.warning_count() == 1; }, 10000ms));
    // More than the link reads in one go: it reads on to the end without another wait's sign.
    for (std::uint8_t i = 0; i < 200; ++i)
    {
        frames.push_back(make_frame(kEcu, kOutside, 0x88B5, 46 + i, i));
        CHECK_EQ(fake.inject("fake-quiet", frames.back()), 1);
    }
    CHECK(eventually([&] { return recorder.frame_count() == frames.size(); }, 10000ms));
    // With nothing to read it looks once for every wait, not in a spin.
    const auto idle_reads = [&](const char* device) {
        const long before = fake.get(device, "reads");
        const auto start = std::chrono::steady_clock::now();
        std::this_thread::sleep_for(500ms);
        const long reads = fake.get(device, "reads") - before;
        const auto waits = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() / 100;
        CHECK(reads >= 1);
        CHECK(reads <= static_cast<long>(waits) + 2);
    };
    idle_reads("fake-quiet");
    link->stop();
    {
        const std::lock_guard lock(recorder.mutex);
        CHECK_EQ(recorder.frames.size(), frames.size());
        for (std::size_t i = 0; i < frames.size() && i < recorder.frames.size(); ++i)
        {
            CHECK(recorder.frames[i] == frames[i]);
        }
        CHECK_EQ(recorder.warnings.size(), std::size_t{1});
        CHECK_TEXT(recorder.warnings.at(0), pcap_no_wake_warning(fake.library->flavor(), "fake-quiet", 100ms));
    }

    // A library that tells: the same look after every wait, and no warning.
    const auto telling = fake.link("fake1");
    Recorder other;
    telling->start(other.handler());
    for (std::uint8_t i = 0; i < 4; ++i)
    {
        CHECK_EQ(fake.inject("fake1", make_frame(kEcu, kOutside, 0x88B5, 46, i)), 1);
        CHECK(eventually([&] { return other.frame_count() == std::size_t{i} + 1; }, 10000ms));
    }
    idle_reads("fake1");
    telling->stop();
    CHECK_EQ(other.warning_count(), std::size_t{0});
}
