// SPDX-License-Identifier: MIT
#include "vehicle_sil/ethernet_bridge/interface_list.hpp"

#include "vehicle_sil/eth_link/external_link.hpp"
#include "vehicle_sil/ethernet_bridge/bridge_app.hpp"
#include "vehicle_sil/json/json_writer.hpp"

#include <cstddef>
#include <cstdint>

namespace vehicle_sil::ethernet_bridge {
namespace {

constexpr std::string_view kTool = "vehicle_sil_ethernet_bridge";

void optional_string(json::JsonWriter& w, std::string_view key, std::string_view value)
{
    if (value.empty())
    {
        w.null(key);
    }
    else
    {
        w.string(key, value);
    }
}

/// The members before "targets".
void begin(json::JsonWriter& w, std::optional<Backend> backend, std::string_view source)
{
    w.begin_object();
    w.integer("vehicle_sil_targets", kTargetsFormat);
    w.string("tool", kTool);
    if (backend)
    {
        w.string("backend", to_string(*backend));
    }
    else
    {
        w.null("backend");
    }
    optional_string(w, "source", source);
    w.begin_array("targets");
}

/// The members after "targets", and the end.
std::string end(json::JsonWriter& w, const std::vector<std::string>& notes)
{
    w.end_array();
    w.begin_array("notes");
    for (const auto& note : notes)
    {
        w.string("", note);
    }
    w.end_array();
    w.null("error");
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

const bridge_runtime::DevicePeers& peers_at(InUse in_use, std::size_t index)
{
    static const bridge_runtime::DevicePeers kNone;
    return index < in_use.size() ? in_use[index] : kNone;
}

/// The texts that end the lines of the interfaces in use.
std::vector<std::string> in_use_notes(InUse in_use)
{
    std::vector<std::string> notes;
    for (const auto& peers : in_use)
    {
        notes.push_back(bridge_runtime::in_use_text(peers));
    }
    return notes;
}

/// The members every target has. `reason`: empty where the target is usable.
void target(json::JsonWriter& w, std::string_view value, std::string detail, std::string_view reason,
            const bridge_runtime::DevicePeers& peers)
{
    const std::string inUse = bridge_runtime::in_use_text(peers);
    if (!inUse.empty())
    {
        detail += "; " + inUse;
    }
    w.string("value", value);
    w.string("label", value);
    w.string("detail", detail);
    w.boolean("usable", reason.empty());
    optional_string(w, "reason", reason);
    w.boolean("in_use", peers.count > 0);
    w.begin_array("in_use_by");
    for (const auto pid : peers.pids)
    {
        w.integer("", pid);
    }
    w.end_array();
    w.integer("in_use_count", static_cast<std::int64_t>(peers.count));
}

} // namespace

std::string format_os_interface_list(const std::vector<eth_link::InterfaceInfo>& interfaces, InUse in_use)
{
    return "interfaces of this computer (--backend packet and --backend pcap take the Ethernet ones; --backend tap takes a TAP "
           "device):\n" +
           eth_link::format_interface_list(interfaces, in_use_notes(in_use));
}

std::string format_pcap_list(const eth_link::PcapInterfaceList& list, InUse in_use)
{
    return eth_link::format_pcap_interface_list(list, in_use_notes(in_use));
}

std::string os_interfaces_json(Backend backend, const std::vector<eth_link::InterfaceInfo>& interfaces, InUse in_use)
{
    json::JsonWriter w;
    begin(w, backend, {});
    for (std::size_t i = 0; i < interfaces.size(); ++i)
    {
        const auto& entry = interfaces[i];
        w.begin_object();
        const std::string why =
            entry.ethernet ? std::string() : "not Ethernet (link type " + std::to_string(entry.link_type) + ")";
        target(w, entry.name, eth_link::interface_detail(entry), why, peers_at(in_use, i));
        w.begin_object("ethernet");
        w.string("name", entry.name);
        w.boolean("ethernet", entry.ethernet);
        w.integer("link_type", entry.link_type);
        w.boolean("up", entry.up);
        w.end_object();
        w.end_object();
    }
    return end(w, {});
}

std::string pcap_interfaces_json(Backend backend, const eth_link::PcapInterfaceList& list, InUse in_use)
{
    json::JsonWriter w;
    begin(w, backend, list.version);
    for (std::size_t i = 0; i < list.devices.size(); ++i)
    {
        const auto& device = list.devices[i];
        // libpcap on Linux offers "any", which captures on every interface at once: no Ethernet.
        const std::string_view why = device.loopback()                                     ? "loopback: not an Ethernet adapter"
                                     : backend == Backend::Pcap && device.name == "any" ? "not Ethernet"
                                                                                           : "";
        w.begin_object();
        target(w, eth_link::pcap_interface_argument(list.devices, i), eth_link::pcap_device_detail(device), why,
               peers_at(in_use, i));
        w.begin_object("ethernet");
        w.string("pcap_name", device.name);
        optional_string(w, "connection", device.connection);
        optional_string(w, "description", device.description);
        w.boolean("up", device.up());
        if (const auto connected = device.connected())
        {
            w.boolean("connected", *connected);
        }
        else
        {
            w.null("connected");
        }
        w.boolean("loopback", device.loopback());
        w.boolean("wireless", device.wireless());
        w.end_object();
        w.end_object();
    }
    return end(w, list.notes);
}

std::string_view interface_list_error_code(const std::exception& error) noexcept
{
    if (const auto* link = dynamic_cast<const eth_link::LinkError*>(&error))
    {
        switch (link->kind())
        {
        case eth_link::LinkErrorKind::LibraryMissing: return "library-missing";
        case eth_link::LinkErrorKind::PermissionDenied: return "permission-denied";
        default: return "failed";
        }
    }
    if (dynamic_cast<const BackendUnavailableError*>(&error) != nullptr)
    {
        return "backend-unavailable";
    }
    return "failed";
}

std::string interface_list_error_json(std::optional<Backend> backend, const std::exception& error)
{
    json::JsonWriter w;
    begin(w, backend, {});
    w.end_array();
    w.begin_array("notes");
    w.end_array();
    w.begin_object("error");
    w.string("code", interface_list_error_code(error));
    w.string("message", error.what());
    w.end_object();
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil::ethernet_bridge
