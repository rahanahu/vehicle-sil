// SPDX-License-Identifier: MIT
//
// The tools of a run that its parts share and the tests read: the participants' arguments, the
// waits, the registry's addresses, the counts and link lines of the bridges of --connect, the
// lines and log names of the participants. Internal.
#pragma once

#include "vehicle_sil/config/check.hpp"
#include "vehicle_sil/config/config.hpp"
#include "vehicle_sil/runner/run_controls.hpp"
#include "vehicle_sil/runner/run_outcome.hpp"
#include "vehicle_sil/runner/run_plan.hpp"
#include "vehicle_sil/process/process.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace vehicle_sil::launch {

/// Where the registry listens if the start waits for a participant someone starts by hand and the
/// file has no registry_uri: a fixed port, so that the command to start it is the same in every
/// run (the port SIL Kit's own registry takes by default).
inline constexpr std::string_view kManualRegistryUri = "silkit://127.0.0.1:8500";

namespace detail {
/// The check that run_file() runs with: a copy of `check` (that passed) with one participant at the
/// end for each network that --connect connects (CheckReport::outside, in its order): the bridge
/// bridge.<network> (role: bridge, required: false, its network's line), with a
/// ParticipantResult that is NotChecked and has the bridge's executable. Without --connect the copy
/// is the same as `check`.
[[nodiscard]] CheckReport with_bridges(const CheckReport& check);

/// The --start-timeout-ms the launcher gives an ecu_main executable: longer than the launcher's own
/// wait for the start, so that the launcher, not the ECU, decides that the start failed and names
/// who did not join; the ECU's limit only ends it if the launcher is gone. kMaxMillis if the start
/// waits for a participant someone starts by hand (waits_for_manual()) or, with
/// `stands_still_before_start`, stands still before the first step until a command (--start-paused
/// with a control server): either may take any time, and the ECU is not Running until it is over.
/// The launcher's own limits (before and after the wait) still end a start that does not come.
[[nodiscard]] std::int64_t ecu_start_timeout_ms(const SimulationConfig& config, bool stands_still_before_start);

/// The arguments that run starts the participant with, after its executable; the command to start
/// it by hand (manual) and the events show the same ones. An ecu_main executable (describe:
/// true): its command's arguments, then --name, --registry-uri `registry_uri` and
/// --start-timeout-ms (ecu_start_timeout_ms() with `stands_still_before_start`), then
/// map_network_arguments() and j1939_node_arguments() of its entry. Any other: its command's
/// arguments with the placeholders replaced, then the same two (a --map-network or --j1939-node
/// after what the file wrote, never inside it).
[[nodiscard]] std::vector<std::string> participant_arguments(const SimulationConfig& config, const ParticipantConfig& participant,
                                                             const std::string& registry_uri,
                                                             bool stands_still_before_start);

/// One line for this system's shell that starts `executable` (as given) with `arguments`: on
/// Windows for cmd.exe, quoted as the C runtimes read a command line back (an argument in double
/// quotes if it has a space, a tab, a quote or one of &|<>^,;, for cmd.exe and PowerShell; %...% is
/// not protected), elsewhere as
/// POSIX sh reads it (an argument in single quotes unless it has only letters, digits and
/// "%+,-./:=@_").
[[nodiscard]] std::string shell_command_line(const std::string& executable, const std::vector<std::string>& arguments);

/// How a wait of the launcher ended.
enum class Waited
{
    Done,
    TimedOut,
    Interrupted,
};
/// Waits up to `timeout` until `done()`, looking every `poll`; stops waiting once `interrupted()`.
/// What has become done by the time the interrupt is seen counts as done: an interrupt that comes
/// just as the awaited thing happens does not change the result.
template <class Done, class Interrupted>
[[nodiscard]] Waited wait_for(std::chrono::milliseconds timeout, std::chrono::milliseconds poll, Done done,
                              Interrupted interrupted)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done())
    {
        if (interrupted())
        {
            return done() ? Waited::Done : Waited::Interrupted;
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return Waited::TimedOut;
        }
        std::this_thread::sleep_for(poll);
    }
    return Waited::Done;
}

/// What the launcher learnt when it asked a running participant to stop.
enum class StopRequest
{
    Sent,        ///< the request went out: wait for the participant
    HadEnded,    ///< it had ended just before (recorded as Ending::Exited)
    CannotSend,  ///< it runs, but the request cannot be sent (Windows: no console): end it now
};
/// Records in `run` what `sent` (Process::request_stop()) and `status` (Process::poll() after it)
/// say.
StopRequest record_stop_request(ParticipantRun& run, bool sent, const std::optional<process::ExitStatus>& status);

/// The registry's URI as the participants on this computer connect to it: a registry that listens
/// on every address ("silkit://0.0.0.0:<port>", "silkit://[::]:<port>") is reached at the loopback
/// address; any other URI as it is.
[[nodiscard]] std::string uri_for_participants(std::string uri);
/// Tells, while the simulation runs, that its virtual time stands still although it is not paused:
/// a participant lost its network connection (nobody notices that by itself for minutes), stands
/// still in a debugger, or hangs in a step. look() once per look of the run's watch.
class StandStill
{
public:
    /// When it has stood still this long (wall-clock time), look() says so; each once.
    static constexpr std::int64_t kSayAfterMs[] = {10000, 60000, 600000};

    /// `now_ms`: the controller's virtual time; `paused`: it stands still on purpose (a pause, a
    /// step); `overdue`: at a set speed, how long the next step is past its deadline (it stands
    /// still between deadlines on purpose), empty as fast as possible; `wall`: the steady clock.
    /// The line to write, or "" (also once it moves again after a line: then it says so).
    [[nodiscard]] std::string look(std::int64_t now_ms, bool paused, std::optional<std::chrono::nanoseconds> overdue,
                                   std::chrono::steady_clock::time_point wall);
    /// The simulation is no longer running (it stops): only the line that it moved again, if it
    /// did after a line; from then on, nothing.
    [[nodiscard]] std::string stopping(std::int64_t now_ms, std::chrono::steady_clock::time_point wall);

private:
    std::int64_t last_ms_ = -1;
    std::chrono::steady_clock::time_point since_{};
    std::size_t said_ = 0;
    bool over_ = false; ///< stopping() was called: nothing more
};

/// Tells which participants outside the file wait for the simulation's start (a mistyped --name):
/// one in Error, or in ServicesCreated for kHold (an autonomous participant passes that state at
/// once on its way to Running). look() for each such participant at each look of the start's wait.
class StrangerWait
{
public:
    enum class State
    {
        ServicesCreated,
        Error,
        Other, ///< any other state, or none known
    };
    static constexpr std::chrono::milliseconds kHold{2000};

    /// Whether `name`, in `state` at `now`, waits for the start.
    [[nodiscard]] bool look(const std::string& name, State state, std::chrono::steady_clock::time_point now);

private:
    std::map<std::string, std::chrono::steady_clock::time_point, std::less<>> since_;
};

/// "a.b.c.d" of the IPv4 address `bytes` (4 bytes, network order); "" for one that other computers
/// do not reach (0.0.0.0/8, loopback 127/8, link-local 169.254/16).
[[nodiscard]] std::string reachable_ipv4_text(const std::uint8_t* bytes);
/// "[...]" of the IPv6 address `bytes` (16 bytes); "" for one that other computers do not reach
/// (::, ::1, link-local fe80::/10, multicast, an IPv4 address written as IPv6).
[[nodiscard]] std::string reachable_ipv6_text(const std::uint8_t* bytes);
/// The names of the network interfaces with a default route, from the text of /proc/net/route and
/// /proc/net/ipv6_route (Linux; either may be empty): each once, in the order found.
[[nodiscard]] std::vector<std::string> default_route_interfaces(std::string_view ipv4_routes, std::string_view ipv6_routes);

/// An address of this computer that other computers may reach.
struct HostAddress
{
    std::string host;           ///< a.b.c.d, or [...] for IPv6
    bool default_route = false; ///< its interface has the default route (most likely the one to give)
};

/// This computer's addresses that other computers may reach: of the network interfaces that are up
/// and running (Windows: operational), without loopback and link-local addresses, each once;
/// those of an interface with a default route first (a virtual one of WSL, Docker or a VM rarely
/// has it), IPv4 before IPv6, otherwise in the system's order. Empty if there are none or they
/// cannot be read. `ipv4`, `ipv6`: which of them.
[[nodiscard]] std::vector<HostAddress> host_addresses(bool ipv4, bool ipv6);
/// `addresses` in the order host_addresses() gives: those with the default route first, IPv4 before
/// IPv6, otherwise as they are.
[[nodiscard]] std::vector<HostAddress> in_order(std::vector<HostAddress> addresses);
/// Whether a registry that listens on [::] here takes IPv4 connections too: Linux unless
/// net.ipv6.bindv6only is 1; not on Windows (IPV6_V6ONLY is set there by default, and SIL Kit keeps it).
[[nodiscard]] bool ipv6_any_takes_ipv4();
/// The line run writes after "registry at" when other computers may reach the registry: where a
/// participant on another computer joins. `uri` is where the registry listens; one on every address
/// ("silkit://0.0.0.0:<port>", "silkit://[::]:<port>") is reached at one of `addresses`
/// (host_addresses()), or, without any, at an address of this computer the line tells how to
/// find; any other URI as it is. `ipv6_only`: a registry on [::] takes no IPv4 (the line says
/// how to take it, with the port the file has: `written_port`, or the port of `uri` if empty).
[[nodiscard]] std::string other_computers_line(const std::string& uri, const std::vector<HostAddress>& addresses,
                                               bool ipv6_only = false, const std::string& written_port = {});
/// What run adds, in parentheses, to "the registry cannot listen at <listen>: <SIL Kit's message>":
/// the likely cause and what to write instead. SIL Kit's message names no cause when the system
/// refuses the address or the port (SIL Kit v5.0.8 only logs it), so the cause is found from
/// `listen`: an IPv4 address that is none of `addresses` (host_addresses()), 0.0.0.0 or loopback
/// is not this computer's; with port 0 no other program can have the port, so only the host can be
/// wrong; with a fixed port of this computer, another registry may have it. `written`: `listen` is
/// the file's registry_uri; `fixed`: it is the port of a run with a participant started by hand.
/// "" when there is nothing to add.
[[nodiscard]] std::string registry_listen_hint(const std::string& listen, bool written, bool fixed,
                                               const std::vector<HostAddress>& addresses);
/// What a bridge of --connect has counted: the frames it sent to the real bus, and those that came
/// from a device there (RunOutside::to_outside, from_outside).
struct OutsideCounts
{
    std::uint64_t to_outside = 0;
    std::uint64_t from_outside = 0;

    friend bool operator==(const OutsideCounts&, const OutsideCounts&) = default;
};

/// The counts in one line of the bridge `tool` (bridge_tool()), or empty. The line is a statistics
/// line of the bridge ("stats key=N key=N ...", after the tag the bridge puts first, such as
/// "[eth-bridge] "; a "\r" at its end does not count) with the keys of `tool`: silkit_to_ext and
/// ext_from_devices (the same for both bridges). Empty for any other line, for a line with a key twice, with a
/// value that is not a decimal number up to 2^63 - 1 or with a part of another form, for one without
/// either key (a bridge of another version), and for any other tool.
[[nodiscard]] std::optional<OutsideCounts> read_outside_counts(std::string_view tool, std::string_view line);

/// Whether `line` is the link line of a bridge ("link key=value ...", after the tag the bridge puts
/// first; see "Link line" in docs/bridge-contract.md). Run does not show these lines: they are for the
/// launcher to read, like the statistics lines.
[[nodiscard]] bool is_link_line(std::string_view line);

/// Every key=N of a statistics line of the bridge `tool`: what read_outside_counts() accepts of the
/// line (the same refusals), with all its keys instead of two. Empty for any other line.
[[nodiscard]] std::optional<std::map<std::string, std::uint64_t>> read_outside_stats(std::string_view tool,
                                                                                    std::string_view line);

/// The state of the link and of the bus that a link line says ("link link=down", "link bus=bus-off").
enum class OutsideLinkValue { Unknown, Up, Down };
enum class OutsideBusValue { Unknown, Active, Passive, BusOff };
/// The keys of a link line that this launcher knows; a key it does not know or a value it does not
/// know is left out: empty.
struct OutsideLinkLine
{
    std::optional<OutsideLinkValue> link{};
    std::optional<OutsideBusValue> bus{};
    /// device=<hash>, marked=0|1, shared=<n> and peers=<pid>,<pid> (Linux): the other
    /// vehicle-sil bridges on the same device (docs/bridge-contract.md, "Device in use by another bridge").
    /// `peers` is read with `shared` and is empty when the line has none.
    std::optional<std::string> device{};
    std::optional<bool> marked{};
    std::optional<std::uint32_t> shared{};
    std::vector<long> peers{};
};
/// The known keys of a link line, or empty for a line that is none (is_link_line()) or whose parts
/// are not "key=value". A part of another form or a repeated key is skipped.
[[nodiscard]] std::optional<OutsideLinkLine> read_outside_link_line(std::string_view line);

/// What the counts of a bridge say about the frames it lost. The one table of the keys of both
/// bridges (names they have in common are listed once); they are the bridges' own names and change
/// with them. Dropped: the queue was full, a send failed, a receive overran, a frame was refused. The others (own_tx_dropped, filtered_*,
/// mac_unguarded, oversize, malformed, ext_checksum_incomplete) are not losses and are only carried.
inline constexpr std::array<std::string_view, 9> kOutsideDroppedKeys{
    "ext_overruns", "ext_queue_full", "ext_tx_failures", "silkit_tx_failures", "overruns",
    "ext_tx_dropped", "ext_tx_unsent", "silkit_rejected", "ext_rejected"};
/// The keys of the two counts that read_outside_counts() reads (both bridges): the frames sent to the
/// bus, and those that came from a device there.
inline constexpr std::string_view kOutsideToKey = "silkit_to_ext";
inline constexpr std::string_view kOutsideFromKey = "ext_from_devices";
/// The bridge sent frames to the bus and none came from a device there: the condition of the note
/// connect-nothing-received (at the end of the run) and of `nothing_received` of the state (while it
/// runs, once kOutsideNothingReceivedAfter has passed since the first frame was sent). The one place
/// that says it; empty counts (none could be read) are not it.
[[nodiscard]] inline bool sent_without_answer(std::optional<std::uint64_t> to_outside, std::optional<std::uint64_t> from_outside)
{
    return to_outside && from_outside && *to_outside != 0 && *from_outside == 0;
}
/// How long after the first frame to the bus nothing from a device makes `nothing_received` true.
inline constexpr std::chrono::milliseconds kOutsideNothingReceivedAfter{10000};
/// How long a count of lost frames or of stopped loops that grew keeps the condition "degraded"
/// (reasons frames-dropped, loop-suspected), and a bus that went passive is not meant: that one
/// lasts as long as the bus is passive.
inline constexpr std::chrono::milliseconds kOutsideDegradedHold{5000};
/// The count of frames the bridge stopped as a loop (Ethernet).
inline constexpr std::string_view kOutsideLoopKey = "loop_suspected";
/// The count of error frames the CAN controller saw.
inline constexpr std::string_view kOutsideBusErrorsKey = "error_frames";
/// The keys of `counts` in kOutsideDroppedKeys whose count is not 0, with the counts, in
/// the order of the table.
[[nodiscard]] std::vector<std::pair<std::string, std::uint64_t>> dropped_counts(const std::map<std::string, std::uint64_t>& counts);

/// Whether `line` is the warning that a bridge writes when its device is used by another vehicle-sil
/// bridge ("warning: <device> is also used by another vehicle-sil bridge (process N): ..."). Run
/// does not show it: the launcher tells it as the finding outside-device-shared, once the bridges
/// of this run are subtracted. One function for the form of the line (the bridges' own text,
/// bridges/common/src/device_mark.cpp).
[[nodiscard]] bool is_shared_warning(std::string_view line);

/// What a bridge's link line says of its device (the sharing keys), kept by OutsideState.
struct OutsideSharing
{
    bool seen = false; ///< a link line was read
    std::string device{};
    bool marked = false;
    std::uint32_t shared = 0;
    std::vector<long> peers{};
};
/// What the launcher knows of a bridge of its own, by the participant's index (the bridge's own
/// participant and every other index of the run): whether its process runs now, and its process id
/// (0: not known).
struct OutsideOwn
{
    bool alive = false;
    long pid = 0;
};
/// The bridges on the device of bridge `i` that are not this run's.
struct OutsideForeign
{
    std::uint32_t count = 0;
    std::vector<long> pids{}; ///< their process ids if the bridge reports them (Linux), else empty
};
/// Bridge `index`'s `shared`, less the bridges of this run. With `peers` (Linux), the ids of the
/// bridges this run started are taken from them (even those that have ended since: their mark may
/// still be in a report). Without (Windows), the number of the run's other bridges that are alive,
/// have placed a mark (marked) and report the same `device`. Nothing if a bridge of the run has not
/// reported yet and the count would need it. Pure; `all` and `own` have one entry per participant.
[[nodiscard]] OutsideForeign foreign_sharing(std::size_t index, const std::vector<OutsideSharing>& all,
                                             const std::vector<OutsideOwn>& own);
/// "another vehicle-sil bridge (process 4321)", "2 other vehicle-sil bridges (processes 4321, 4400)"
/// (Windows: no ids, "another vehicle-sil bridge").
[[nodiscard]] std::string shared_text(const OutsideForeign& foreign);

/// What one bridge's link line and counts add up to. Pure like OutsideNote: time is an argument, no
/// clock, no thread, no lock (RunOutput's mutex guards it).
class OutsideState
{
public:
    /// A link line read at `now`. true: link or bus changed (what people are told).
    bool on_link(const OutsideLinkLine& line, std::chrono::steady_clock::time_point now);
    /// The counts of a statistics line read at `now` (the last ones are kept). A sum of lost frames
    /// (kOutsideDroppedKeys) or a count of loops (kOutsideLoopKey) that is larger than before is
    /// noted with `now`, for the condition.
    void on_counts(std::map<std::string, std::uint64_t> counts, std::chrono::steady_clock::time_point now);
    /// What the bridge stands at `now`, for RunState::outside (the names are the caller's): the link,
    /// the bus, `nothing_received`, and the condition with its reasons (docs/simulation-events.md).
    /// interrupted: the link is down or the bus is off. degraded: the bus is passive, frames were
    /// lost or loops stopped within kOutsideDegradedHold, or a bridge outside this run shares the
    /// device (on_foreign()). Both: interrupted, with the reasons of both.
    [[nodiscard]] RunState::Outside facts(std::chrono::steady_clock::time_point now) const;
    /// The link as it is now: "up", "down", "unknown", or empty before any line.
    [[nodiscard]] std::string link_text() const;
    /// The bus as it is now, as link_text().
    [[nodiscard]] std::string bus_text() const;
    /// Why the run must stop at `now`: the link or the bus has been down for `limit` or more
    /// ("link down for 30 s", "bus off for 30 s"), else empty. The link comes first.
    [[nodiscard]] std::string lost(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const;
    /// The sums at `now` (a down or bus-off that is still on counts to `now`), with `lost` empty.
    [[nodiscard]] OutsideLink summary(std::chrono::steady_clock::time_point now) const;
    /// A link line has been read.
    [[nodiscard]] bool link_line_seen() const { return sharing_.seen; }
    /// What the link lines said of the device (foreign_sharing() takes it from every bridge).
    [[nodiscard]] const OutsideSharing& sharing() const { return sharing_; }
    /// The bridges outside this run on the device, as foreign_sharing() has them at `now` (`since_start`
    /// into the run). true: more than ever before, which people are told once.
    bool on_foreign(const OutsideForeign& foreign, std::chrono::steady_clock::time_point now,
                    std::chrono::milliseconds since_start);
    /// The device has been shared with a bridge outside this run for `limit` or more.
    [[nodiscard]] bool shared_for(std::chrono::steady_clock::time_point now, std::chrono::milliseconds limit) const;
    /// The run is stopped (or not started) for it; summary() says so.
    void stop_shared(OutsideLink::SharedStop how) { shared_stop_ = how; }

private:
    struct Span
    {
        std::uint32_t count = 0;
        std::chrono::steady_clock::duration total{};
        std::optional<std::chrono::steady_clock::time_point> since{}; ///< when the one that is on began
        void begin(std::chrono::steady_clock::time_point now);
        void end(std::chrono::steady_clock::time_point now);
        [[nodiscard]] std::chrono::steady_clock::duration total_at(std::chrono::steady_clock::time_point now) const;
    };
    std::optional<OutsideLinkValue> link_{};
    std::optional<OutsideBusValue> bus_{};
    Span down_;
    Span bus_off_;
    Span passive_;
    std::map<std::string, std::uint64_t> counts_;
    OutsideSharing sharing_;
    Span foreign_;
    std::uint64_t dropped_ = 0; ///< the sum of the lost-frame counts of the last statistics line
    std::optional<std::chrono::steady_clock::time_point> dropped_at_{};
    std::uint64_t loops_ = 0;
    std::optional<std::chrono::steady_clock::time_point> loops_at_{};
    std::optional<std::chrono::steady_clock::time_point> sent_since_{}; ///< when frames to the bus were first counted
    std::uint32_t foreign_most_ = 0;
    std::vector<long> foreign_pids_;
    std::optional<std::chrono::milliseconds> foreign_first_{};
    OutsideLink::SharedStop shared_stop_ = OutsideLink::SharedStop::None;
};

/// When run --events writes the event "outside" of one bridge: when its counts have changed
/// (on_counts()), at most once in kEvery; counts held back by that go out once kEvery has passed
/// (due(), which the run asks while it watches the simulation) and at the end (flush()), so the last
/// counts are always written. Time is an argument: no clock, no thread, no lock (the caller's).
class OutsideNote
{
public:
    static constexpr std::chrono::milliseconds kEvery{1000};

    /// Counts read at `now`: what to write now, or empty (the same as written last, or too soon).
    [[nodiscard]] std::optional<OutsideCounts> on_counts(OutsideCounts counts, std::chrono::steady_clock::time_point now);
    /// The counts held back, if kEvery has passed at `now` since the last ones written; else empty.
    [[nodiscard]] std::optional<OutsideCounts> due(std::chrono::steady_clock::time_point now);
    /// The counts held back, whatever the time; else empty.
    [[nodiscard]] std::optional<OutsideCounts> flush();
    /// The counts read last, or empty if none were.
    [[nodiscard]] std::optional<OutsideCounts> last() const { return last_; }

private:
    std::optional<OutsideCounts> last_;
    std::optional<OutsideCounts> written_;
    std::chrono::steady_clock::time_point written_at_{};
};

/// One line of a participant's output as the launcher shows it: control characters (C0 but tabs,
/// DEL, C1) and bytes that are not UTF-8 as '?'.
[[nodiscard]] std::string printable(std::string_view line);
/// The file name (without ".log") of a participant's log: its name with every byte but ASCII
/// letters, digits, '.', '-' and '_' as '_', and a Windows device name (NUL, CON, COM1, ...; also
/// before a '.') with '_' after it.
[[nodiscard]] std::string log_name(const std::string& name);
} // namespace detail

} // namespace vehicle_sil::launch
