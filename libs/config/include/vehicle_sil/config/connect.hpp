// SPDX-License-Identifier: MIT
//
// Connecting a network of the simulation outside, to a real bus (--connect <network>=<target>):
// the request on the command line, the form of a target, the bridge tool of each kind of network,
// the arguments the launcher gives it and the words about where the launcher looks for it. Pure
// functions (the search itself is find_tool() in the launcher's launch/probe.hpp). Internal; the command line: docs/simulation-config.md, the findings:
// docs/simulation-check.md.
#pragma once

#include "vehicle_sil/config/config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace vehicle_sil::config {

/// One --connect <network>=<target>.
struct ConnectRequest
{
    std::string network;
    std::string target;

    friend bool operator==(const ConnectRequest&, const ConnectRequest&) = default;
};

/// Reads "<network>=<target>", split at the first '=' (a network name with '=' cannot be given;
/// the target may have one). Empty if there is no '=' or nothing before it; the target may be
/// empty (target_problem() says so).
[[nodiscard]] std::optional<ConnectRequest> parse_connect(std::string_view text);

/// The target that connects nothing: the network runs on SIL Kit only, as without --connect.
inline constexpr std::string_view kNoTarget = "none";

/// The longest target, in bytes.
inline constexpr std::size_t kMaxTargetBytes = 256;

/// The operating system whose rules a target is read by (the backends it can name, the default).
/// Functions that read a target take it so that the tests can read as either; the default is this
/// computer's.
enum class Os
{
    Linux,
    Windows,
};
#ifdef _WIN32
inline constexpr Os kThisOs = Os::Windows;
#else
inline constexpr Os kThisOs = Os::Linux;
#endif

/// One row of the table of backends: how a network of `kind` is connected outside. A target names
/// one as "<backend>:<selector>" (the selector alone takes the default of the kind on the OS).
struct ConnectBackend
{
    NetworkKind kind;
    std::string_view name; ///< what a target writes before the ':' (lower case)
    std::string_view tool; ///< the bridge executable
    bool on_linux;         ///< a target can name it on Linux
    bool on_windows;       ///< a target can name it on Windows
    bool listed_on_linux;  ///< `connections` lists it on Linux
    bool listed_on_windows;
};

/// The row of `name` for `kind` (a backend of any OS), or null.
[[nodiscard]] const ConnectBackend* find_backend(NetworkKind kind, std::string_view name);

/// The backends that a target can name on `os` for `kind`, in the table's order.
[[nodiscard]] std::vector<const ConnectBackend*> backends_on(NetworkKind kind, Os os);

/// The backends that `connections` lists on `os` for `kind`: those of backends_on() that can be
/// listed there (Vector XL is only listed on Windows, though it can be named on Linux).
[[nodiscard]] std::vector<const ConnectBackend*> listed_backends(NetworkKind kind, Os os);

/// A target read by the grammar `none | [<backend>:]<selector>`. `<backend>:` is read only when
/// the text before the first ':' is the name of a backend of `kind` in the table (lower case, any
/// OS); a ':' after any other word belongs to the selector (`virtual:1`, `eth0:1`), and
/// `packet:none` is an interface named none: only a target without a prefix can be kNoTarget.
struct ConnectTarget
{
    bool none = false;
    bool prefixed = false;        ///< the target wrote the backend
    std::string_view backend;     ///< the backend as written, or the default of `kind` on the OS; empty for `none` and LIN
    std::string_view selector;    ///< what follows the prefix (the whole target without one)
};

/// Reads `target` for a network of `kind` on `os`. The default backend: ethernet: packet on
/// Linux, npcap on Windows; can: xl if the selector is virtual:<n> or physical:<n>, else
/// socketcan on Linux and xl on Windows. The result views into `target`.
[[nodiscard]] ConnectTarget read_connect_target(NetworkKind kind, std::string_view target, Os os = kThisOs);

/// The executable of `backend` for `kind` ("" if the table has none), as in the table.
[[nodiscard]] std::string_view bridge_tool(NetworkKind kind, std::string_view backend);

/// true if `tool` is one of the bridge executables of the table.
[[nodiscard]] bool is_bridge_tool(std::string_view tool);

/// true if `tool` is one of the bridge executables of the can backends.
[[nodiscard]] bool is_can_bridge_tool(std::string_view tool);

/// Why a network of `kind` has no bridge for `backend` on `os` ("" is any backend of the kind), or
/// empty: "there is no bridge for LIN", "the Vector XL bridge is for Windows".
[[nodiscard]] std::optional<std::string> no_bridge_here(NetworkKind kind, std::string_view backend = {},
                                                        Os os = kThisOs);

/// What is wrong with the backend that `target` writes (connect-backend), or empty: a backend of
/// the table that `os` cannot use. The text lists the backends of `os` and says where to see the
/// targets.
[[nodiscard]] std::optional<std::string> backend_problem(NetworkKind kind, std::string_view target, Os os = kThisOs);

/// The working directory of a bridge that the launcher starts (its --describe, its list, its run):
/// the directory of its executable `tool`. Never the directory of the configuration file or the
/// launcher's working directory: a DLL put into a folder received from someone is not loaded.
[[nodiscard]] std::filesystem::path bridge_working_directory(const std::filesystem::path& tool);

/// The participant name of the bridge of `network`: "bridge.<network>".
[[nodiscard]] std::string bridge_participant(std::string_view network);

/// What is wrong with `target` for a network of `kind`, or empty: backend_problem(), it is empty,
/// longer than kMaxTargetBytes, not UTF-8, has a control character (C0, DEL, C1, or one of
/// Unicode's that change the direction of text: U+202A to U+202E, U+2066 to U+2069), the selector
/// is empty or starts with '-'; with the xl backend the selector is not virtual:<n> or
/// physical:<n> with n from 1. kNoTarget is never wrong.
[[nodiscard]] std::optional<std::string> target_problem(NetworkKind kind, std::string_view target, Os os = kThisOs);

/// The arguments of the bridge of `network` at `target` (target_problem() empty), without --name
/// and what a run adds. ethernet: --network, --backend, --interface, and the frames that pass:
/// --all-frames or --ethertype (external) or else --src-mac with `macs` (in their order, each
/// once; none if `macs` is empty). can with xl: --network, --xl-target, --xl-channel and --bitrate
/// (external); with socketcan: --network, --interface and --bitrate (external).
[[nodiscard]] std::vector<std::string> bridge_arguments(const NetworkConfig& network, std::string_view target,
                                                        const std::vector<eth::MacAddress>& macs, Os os = kThisOs);

/// The device that `target` names for a network of `kind`, as a string to compare, or empty if it
/// names none (kNoTarget, or a target that target_problem() refuses). Two networks whose targets
/// have the same key use the same device. The backend is left out: `enp3s0`, `packet:enp3s0` and
/// `tap:enp3s0` are one key. Ethernet: the interface name (lower case on Windows). CAN: "physical:<n>"
/// per xl channel number; every xl "virtual:<n>" is "virtual" (the Vector Virtual CAN channels are
/// one bus); the name of a SocketCAN interface. Strings only: a connection name and the pcap name
/// of the same adapter are not told apart.
[[nodiscard]] std::string connect_device_key(NetworkKind kind, std::string_view target, Os os = kThisOs);

/// true if the networks `a` and `b` are in one connected group when each entry of
/// `participant_networks` (the networks one participant is on) joins its networks: they are on
/// one participant, or on a chain of participants (a-G1-c-G2-b).
[[nodiscard]] bool networks_are_joined(const std::vector<std::vector<std::string>>& participant_networks,
                                       const std::string& a, const std::string& b);

namespace detail {
/// The length in bytes of the Unicode control character at the start of UTF-8 `text`: a C1
/// control (U+0080 to U+009F: 2) or a control of the direction of text (U+202A to U+202E, U+2066
/// to U+2069: 3); 0 for anything else. A terminal does not show them as they are.
[[nodiscard]] std::size_t unicode_control_at(std::string_view text) noexcept;
/// true if `text` has a character of unicode_control_at().
[[nodiscard]] bool has_unicode_control(std::string_view text) noexcept;
} // namespace detail

/// Where find_tool() looked and found nothing, for a message: "neither next to
/// vehicle_sil_launch (<launcher_dir>) nor on PATH", or "not on PATH" if `launcher_dir` is empty.
[[nodiscard]] std::string tool_not_found(const std::filesystem::path& launcher_dir);

/// What to do about the bridge executable `tool` that find_tool() does not find: build it (where
/// the docs say how) and put it where the launcher looks.
[[nodiscard]] std::string tool_missing_fix(std::string_view tool);

} // namespace vehicle_sil::config
