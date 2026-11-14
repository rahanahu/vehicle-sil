// SPDX-License-Identifier: MIT
//
// Windows: loads Npcap's wpcap.dll and waits for packets on the event the library gives. Only
// loading, waiting and the names of the network connections are here; the link's logic is in
// pcap_link.cpp.
//
// Where the library is loaded from: <system directory>\Npcap\wpcap.dll, by its full path. The
// DLLs wpcap.dll itself needs (Packet.dll) are looked for next to it and then in the system
// directory, and nowhere else (kLibrarySearch). Not the current directory, not PATH, not the
// directory of the program, for wpcap.dll and for what it needs alike: a DLL that came with a
// folder of files from someone else is never loaded. (The system directory can be written by
// administrators only.)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
// clang-format on

#include "pcap_api.hpp"

#include <algorithm>
#include <cstddef>

// Older headers of the toolchain may lack these (Windows has them from 8 on, and 7 with an update).
#ifndef LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR
#define LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR 0x00000100
#endif
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800
#endif

namespace vehicle_sil::eth_link::detail {
namespace {

// The library's packet header begins with the struct timeval of this OS's headers (winsock2.h).
static_assert(sizeof(timeval) == 2 * sizeof(long) && offsetof(timeval, tv_usec) == offsetof(PcapPacketHeader, microseconds) &&
                  offsetof(PcapPacketHeader, captured_bytes) == sizeof(timeval) && sizeof(PcapPacketHeader) == sizeof(timeval) + 8,
              "PcapPacketHeader (pcap_api.hpp) does not begin like this system's struct timeval: write it for this system");

/// Where Windows looks for the DLLs the library needs: the library's own directory, then the
/// system directory. Without these flags it would go on to the current directory and PATH. A DLL
/// that is in the process already is used wherever it came from (Windows does not search then).
constexpr DWORD kLibrarySearch = LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32;

/// How long a wait lasts when the library has no event to wait on.
constexpr DWORD kWaitWithoutEventMs = 10;

DWORD milliseconds_of(std::chrono::milliseconds time) noexcept
{
    return static_cast<DWORD>(std::clamp<std::chrono::milliseconds::rep>(time.count(), 0, 0x7fffffff));
}

/// Text of Windows in the code page the program's arguments and its console output use.
std::string narrow(const wchar_t* text)
{
    if (text == nullptr)
    {
        return {};
    }
    const int size = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1)
    {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_ACP, 0, text, -1, result.data(), size, nullptr, nullptr);
    result.resize(static_cast<std::size_t>(size) - 1);
    return result;
}

std::wstring widen(const std::string& text)
{
    const int size = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (size <= 1)
    {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, result.data(), size);
    result.resize(static_cast<std::size_t>(size) - 1);
    return result;
}

/// `path` with its drive and directory, and with backslashes (what LoadLibraryEx asks for).
std::wstring full_path(const std::wstring& path)
{
    wchar_t buffer[32768];
    const DWORD size = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    return size > 0 && size < std::size(buffer) ? std::wstring(buffer, size) : path;
}

/// <system directory>\Npcap\wpcap.dll, or empty if Windows does not tell its system directory.
std::wstring installed_library()
{
    wchar_t directory[MAX_PATH];
    const UINT size = GetSystemDirectoryW(directory, MAX_PATH);
    if (size == 0 || size >= MAX_PATH)
    {
        return {};
    }
    return std::wstring(directory, size) + L"\\Npcap\\wpcap.dll";
}

class WindowsWaiter final : public PcapWaiter
{
public:
    /// `packets`: the library's event, signalled when a packet is there, or null.
    explicit WindowsWaiter(HANDLE packets) : packets_(packets), stop_(CreateEventW(nullptr, TRUE, FALSE, nullptr))
    {
        if (stop_ == nullptr)
        {
            throw LinkError(LinkErrorKind::Other,
                            "cannot start the pcap link (CreateEvent: Windows error " + std::to_string(GetLastError()) + ").");
        }
    }
    ~WindowsWaiter() override { CloseHandle(stop_); }

    PcapWake wait(std::chrono::milliseconds limit) override
    {
        if (packets_ == nullptr || packets_ == INVALID_HANDLE_VALUE)
        {
            // The only way to know is to read.
            return pause(std::chrono::milliseconds(std::min<DWORD>(milliseconds_of(limit), kWaitWithoutEventMs))) ? PcapWake::Stop
                                                                                                                 : PcapWake::Ready;
        }
        const HANDLE handles[2] = {stop_, packets_};
        const DWORD result = WaitForMultipleObjects(2, handles, FALSE, milliseconds_of(limit));
        if (result == WAIT_OBJECT_0)
        {
            return PcapWake::Stop;
        }
        // A failed wait is "ready" too: reading tells what is wrong.
        return result == WAIT_TIMEOUT ? PcapWake::TimedOut : PcapWake::Ready;
    }
    bool pause(std::chrono::milliseconds time) override
    {
        return WaitForSingleObject(stop_, milliseconds_of(time)) == WAIT_OBJECT_0;
    }
    void request_stop() noexcept override { SetEvent(stop_); }

private:
    const HANDLE packets_;
    const HANDLE stop_;
};

class WindowsLibrary final : public PcapLibrary
{
public:
    WindowsLibrary(HMODULE module, std::string path, PcapFlavor flavor) : PcapLibrary(flavor, std::move(path)), module_(module)
    {
        try
        {
            bind(PcapWaitKind::Event);
        }
        catch (...)
        {
            FreeLibrary(module_);
            throw;
        }
    }
    ~WindowsLibrary() override { FreeLibrary(module_); }

    [[nodiscard]] std::unique_ptr<PcapWaiter> make_waiter(PcapHandle* handle) const override
    {
        return std::make_unique<WindowsWaiter>(static_cast<HANDLE>(api().getevent(handle)));
    }

    [[nodiscard]] std::unique_ptr<PcapInterfaceWatch> make_watch(const std::string&) const override { return nullptr; }

    /// The names Windows shows for the network connections ("Ethernet 2"), by adapter identifier.
    [[nodiscard]] std::vector<PcapConnectionName> connection_names() const override
    {
        std::vector<PcapConnectionName> names;
        const ULONG flags = GAA_FLAG_SKIP_UNICAST | GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
        ULONG size = 16 * 1024;
        std::vector<ULONG64> buffer; // 8 byte units: the alignment the adapter structures need
        ULONG result = ERROR_BUFFER_OVERFLOW;
        for (int attempt = 0; attempt < 4 && result == ERROR_BUFFER_OVERFLOW; ++attempt)
        {
            buffer.assign(size / sizeof(ULONG64) + 1, 0);
            result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
        }
        if (result != NO_ERROR)
        {
            return names; // the devices are listed and chosen by their pcap names then
        }
        for (const IP_ADAPTER_ADDRESSES* adapter = reinterpret_cast<const IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter != nullptr;
             adapter = adapter->Next)
        {
            if (adapter->AdapterName != nullptr)
            {
                names.push_back({adapter->AdapterName, narrow(adapter->FriendlyName)});
            }
        }
        return names;
    }
    [[nodiscard]] void* symbol(const char* name) const noexcept override
    {
        return reinterpret_cast<void*>(GetProcAddress(module_, name));
    }
    [[nodiscard]] std::string executable_path() const override { return {}; }

private:
    const HMODULE module_;
};

std::shared_ptr<PcapLibrary> load(const std::string& library, PcapFlavor flavor)
{
    const std::wstring path = library.empty() ? installed_library() : full_path(widen(library));
    if (path.empty())
    {
        throw pcap_library_missing(flavor, "wpcap.dll", "Windows does not tell where its system directory is");
    }
    // No message box of Windows for a DLL that cannot be loaded: the bridge says it itself.
    const UINT mode = SetErrorMode(SEM_FAILCRITICALERRORS);
    // `path` is a full path, which these flags ask for.
    const HMODULE module = LoadLibraryExW(path.c_str(), nullptr, kLibrarySearch);
    const DWORD error = GetLastError();
    SetErrorMode(mode);
    if (module == nullptr)
    {
        throw pcap_library_missing(flavor, narrow(path.c_str()), "Windows error " + std::to_string(error));
    }
    return std::make_shared<WindowsLibrary>(module, narrow(path.c_str()), flavor);
}

} // namespace

std::shared_ptr<PcapLibrary> load_pcap_library(const std::string& library)
{
    return load(library, PcapFlavor::Npcap);
}

std::shared_ptr<PcapLibrary> load_pcap_library_as(const std::string& library, PcapFlavor flavor)
{
    return load(library, flavor);
}

} // namespace vehicle_sil::eth_link::detail
