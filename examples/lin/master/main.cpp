// SPDX-License-Identifier: MIT
//
// vehicle_sil_lin_master: the master of the example LIN cluster (examples/lin/common/
// lin_cluster.hpp), in virtual time. It runs the schedule, publishes the command frame, and
// sends the cluster to sleep and wakes it at the times given on the command line.
#include "../common/lin_cluster.hpp"

#include "vehicle_sil/lin/lin_master.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/services/lin/all.hpp>

#include <chrono>
#include <cstdio>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;
using namespace vehicle_sil::lin;
using namespace vehicle_sil::lin_example;

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

void print_usage()
{
    std::fputs("usage: vehicle_sil_lin_master [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --network <name>              SIL Kit LIN network (LIN_CLUSTER)\n"
               "  --slot-us <us>                schedule slot of every frame (10000)\n"
               "  --wakeup-delay-ms <ms>        schedule restart after a wakeup pulse (100)\n"
               "  --sleep-at-ms <t,...>         request sleep at these steps\n"
               "  --wakeup-at-ms <t,...>        request wakeup at these steps\n",
               stderr);
}

int run(const Args& args)
{
    const CommonOptions o = parse_common(args, "LinMaster", 5ms);
    const std::string network = args.str("network", "LIN_CLUSTER");
    const SimDuration slot = std::chrono::microseconds(args.u64("slot-us", 10000, 10'000'000));
    LinMasterOptions masterOptions{std::chrono::milliseconds(args.u64("wakeup-delay-ms", 100, 3'600'000))};
    const auto sleepAt = times_ms(args, "sleep-at-ms");
    const auto wakeupAt = times_ms(args, "wakeup-at-ms");
    Trace trace(o.trace_path);

    ParticipantRuntime runtime(runtime_options(o));
    SilKitLinPort port(*runtime.participant().CreateLinController("LIN1", network), LinRole::Master);
    std::unique_ptr<LinMaster> master;
    try
    {
        master = std::make_unique<LinMaster>(port, cluster_schedule(slot), o.step, masterOptions);
    }
    catch (const std::invalid_argument& e)
    {
        throw UsageError(e.what());
    }
    master->on_result([&trace](const LinScheduleResult& r) {
        trace.line("result t_us=%lld id=0x%02X status=%s data=%s", us(r.header_time), r.id, status_name(r.status),
                   hex(r.data.data(), r.data_length).c_str());
    });
    master->on_state([&trace](SimTime t, LinNodeState s) { trace.line("state t_us=%lld state=%s", us(t), to_string(s)); });
    runtime.set_communication_ready_handler([&] { master->init(); });
    runtime.set_step_handler([&](SimTime now, SimDuration) {
        master->set_frame_data(kCommandId, command_at(now));
        if (sleepAt.count(now) != 0)
        {
            master->request_sleep();
        }
        if (wakeupAt.count(now) != 0)
        {
            master->request_wakeup();
        }
        master->step(now);
    });

    const int result = run_until_finished(runtime, o);
    const auto stats = master->statistics();
    log(o.participant,
        "headers=%llu ok=%llu no_response=%llu errors=%llu sleeps=%llu wakeups_by_master=%llu wakeups_by_slaves=%llu "
        "overflows=%llu",
        static_cast<unsigned long long>(stats.headers_sent), static_cast<unsigned long long>(stats.responses_ok),
        static_cast<unsigned long long>(stats.no_responses), static_cast<unsigned long long>(stats.errors),
        static_cast<unsigned long long>(stats.sleep_commands), static_cast<unsigned long long>(stats.wakeups_by_master),
        static_cast<unsigned long long>(stats.wakeups_by_slaves), static_cast<unsigned long long>(stats.inbox_overflows));
    if (result == kExitOk && stats.inbox_overflows != 0)
    {
        return kExitFailed;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "lin-master", print_usage, run);
}
