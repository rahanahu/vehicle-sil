// SPDX-License-Identifier: MIT
//
// vehicle_sil_vt_body_controller: a LIN master that also listens to the J1939 CAN network, in
// virtual time (5 ms step by default), on a vehicle_sil::Peer. It runs a LIN schedule
// (vehicle_sil::lin::LinMaster) and records every frame the J1939 network receives. The J1939
// network has no node of its own: a read-only tap on its port (TapPort) reports each frame the
// step releases, with the frame's time, and the application stage of the same step traces it.
#include "../common/vt_peer.hpp"

#include "vehicle_sil/can/tap_port.hpp"
#include "vehicle_sil/lin/lin_master.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;
using namespace vehicle_sil::lin;

constexpr LinId kDistanceId = 0x20;
constexpr LinId kStatusId = 0x21;

const char* status_name(LinResultStatus status)
{
    switch (status)
    {
    case LinResultStatus::Ok:
        return "ok";
    case LinResultStatus::NoResponse:
        return "no_response";
    case LinResultStatus::Error:
        return "error";
    }
    return "?";
}

/// Collects the frames the J1939 network received in the current step (on the step's thread).
/// The tap sees them in the port's order: by time, and frames with the same time in the order
/// they arrived, which SIL Kit does not fix for different senders.
class ReceivedFrames final : public can::TapListener
{
public:
    void on_tap(const can::TapRecord& record) override
    {
        if (record.direction == can::TapRecord::Direction::Received)
        {
            frames_.push_back({record.time, record.frame});
        }
    }

    struct Received
    {
        SimTime time;
        can::Frame frame;
    };
    /// The frames received since the last call, by (time, CAN identifier): frames of different
    /// senders with the same time in a fixed order (the lower identifier first, as arbitration
    /// would), each sender's frames with the same identifier in the order they came.
    std::vector<Received> take()
    {
        std::stable_sort(frames_.begin(), frames_.end(), [](const Received& a, const Received& b) {
            return a.time != b.time ? a.time < b.time : a.frame.id < b.frame.id;
        });
        return std::exchange(frames_, {});
    }

private:
    std::vector<Received> frames_;
};

void print_usage()
{
    std::fputs("usage: vehicle_sil_vt_body_controller [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --can-network <name>          SIL Kit CAN network (VT_CAN)\n"
               "  --lin-network <name>          SIL Kit LIN network (VT_LIN)\n"
               "  --slot-us <us>                LIN schedule slot of each frame (10000)\n",
               stderr);
}

int run(const Args& args)
{
    const CommonOptions o = parse_common(args, "BodyController", 5ms);
    const std::string canNetwork = args.str("can-network", "VT_CAN");
    const std::string linNetwork = args.str("lin-network", "VT_LIN");
    const SimDuration slot = std::chrono::microseconds(args.u64("slot-us", 10000, 10'000'000));

    // Used by the steps: declared before the peer, so they outlive it.
    Trace trace(o.trace_path);
    ReceivedFrames received;

    Peer peer(peer_options(o));
    // J1939 monitor: the network releases the frames sent before the step; the tap reports them.
    J1939ChannelOptions channel;
    channel.controller_name = "CAN1";
    const auto& bus = peer.j1939(canNetwork, channel);
    peer.tap(canNetwork).set_listener(&received);
    // LIN master: distance and status frames, one slot each. A result is reported at the end of
    // its slot, so results whose slot ends at or after --end-ms are not reported.
    LinMaster* master = nullptr;
    try
    {
        master = &peer.lin_master(linNetwork, LinScheduleTable{{kDistanceId, 3, LinChecksumModel::Enhanced, slot},
                                                               {kStatusId, 1, LinChecksumModel::Enhanced, slot}});
    }
    catch (const std::invalid_argument& e)
    {
        throw UsageError(e.what());
    }
    master->on_result([&trace](const LinScheduleResult& r) {
        trace.line("lin header_us=%lld id=0x%02X status=%s data=%s", us(r.header_time), r.id, status_name(r.status),
                   hex(r.data.data(), r.data_length).c_str());
    });
    peer.on_step([&](SimTime now) {
        for (const auto& r : received.take())
        {
            // J1939 uses 29-bit data frames only; the network ignores anything else.
            if (r.frame.format != can::IdFormat::Extended)
            {
                continue;
            }
            trace.line("can rx_us=%lld ts_us=%lld id=0x%08X data=%s", us(now), us(r.time), static_cast<unsigned>(r.frame.id),
                       hex(r.frame.payload().data(), r.frame.payload().size()).c_str());
        }
    });

    const int result = run_until_finished(peer, o);
    const auto stats = master->statistics();
    // The network's statistics change only in steps, so they are those of the handled steps.
    const std::uint64_t overflows = bus.statistics().receive_overflows + bus.statistics().port_receive_overflows;
    log(o.participant, "lin headers=%llu ok=%llu no_response=%llu errors=%llu can_overflows=%llu",
        static_cast<unsigned long long>(stats.headers_sent), static_cast<unsigned long long>(stats.responses_ok),
        static_cast<unsigned long long>(stats.no_responses), static_cast<unsigned long long>(stats.errors),
        static_cast<unsigned long long>(overflows));
    if (result == kExitOk && (overflows != 0 || stats.inbox_overflows != 0))
    {
        return kExitFailed;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "body-controller", print_usage, run);
}
