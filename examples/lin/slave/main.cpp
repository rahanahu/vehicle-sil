// SPDX-License-Identifier: MIT
//
// vehicle_sil_lin_slave: a slave of the example LIN cluster (examples/lin/common/
// lin_cluster.hpp), in virtual time. It receives the master's command frame and, depending on
// --role, publishes the echo or the status frame. It can wake the cluster at given times.
#include "../common/lin_cluster.hpp"

#include "vehicle_sil/lin/lin_slave.hpp"
#include "vehicle_sil/lin_silkit/silkit_lin_port.hpp"

#include <silkit/services/lin/all.hpp>

#include <array>
#include <chrono>
#include <cstdio>
#include <memory>

namespace {

using namespace std::chrono_literals;
using namespace vehicle_sil;
using namespace vehicle_sil::vt;
using namespace vehicle_sil::lin;
using namespace vehicle_sil::lin_example;

void print_usage()
{
    std::fputs("usage: vehicle_sil_lin_slave [options]\n", stderr);
    std::fputs(kCommonUsage, stderr);
    std::fputs("  --role echo|status            the frame this slave publishes (required)\n"
               "  --network <name>              SIL Kit LIN network (LIN_CLUSTER)\n"
               "  --bus-idle-timeout-ms <ms>    sleep after this much bus silence; 0: never (4000)\n"
               "  --wakeup-at-ms <t,...>        request a wakeup at these steps\n",
               stderr);
}

int run(const Args& args)
{
    const std::string role = args.required("role");
    if (role != "echo" && role != "status")
    {
        throw UsageError("--role must be echo or status");
    }
    const CommonOptions o = parse_common(args, role == "echo" ? "LinEcho" : "LinStatus", 2ms);
    const std::string network = args.str("network", "LIN_CLUSTER");
    LinSlaveOptions slaveOptions;
    slaveOptions.subscriptions = {kCommandId};
    const auto idleMs = args.u64("bus-idle-timeout-ms", 4000, 3'600'000);
    slaveOptions.bus_idle_timeout =
        idleMs == 0 ? std::nullopt : std::optional<SimDuration>(std::chrono::milliseconds(idleMs));
    const auto wakeupAt = times_ms(args, "wakeup-at-ms");
    Trace trace(o.trace_path);

    std::array<std::uint8_t, 2> newestCommand{};
    std::uint8_t commands = 0;
    SimTime currentStep{}; // traced with each frame: the step that handled it

    ParticipantRuntime runtime(runtime_options(o));
    SilKitLinPort port(*runtime.participant().CreateLinController("LIN1", network), LinRole::Slave);
    LinSlave slave(port, slaveOptions);
    slave.on_frame([&](const LinReceivedFrame& f) {
        trace.line("frame t_us=%lld rx_us=%lld id=0x%02X data=%s", us(f.header_time), us(currentStep), f.id,
                   hex(f.data.data(), f.data_length).c_str());
        if (f.id == kCommandId && f.data_length == 2)
        {
            newestCommand = {f.data[0], f.data[1]};
            ++commands;
        }
    });
    slave.on_state([&](SimTime t, LinNodeState s) { trace.line("state t_us=%lld state=%s", us(t), to_string(s)); });
    runtime.set_communication_ready_handler([&] { slave.init(); });
    runtime.set_step_handler([&](SimTime now, SimDuration) {
        const auto pulsesBefore = slave.statistics().wakeups_sent;
        if (wakeupAt.count(now) != 0)
        {
            slave.request_wakeup(); // sends a pulse only if the slave is still asleep in step()
        }
        currentStep = now;
        slave.step(now);
        if (slave.statistics().wakeups_sent != pulsesBefore)
        {
            trace.line("wakeup t_us=%lld", us(now));
        }
        if (role == "echo")
        {
            slave.set_response(now, kEchoId, newestCommand);
        }
        else
        {
            const std::array<std::uint8_t, 1> status{commands};
            slave.set_response(now, kStatusId, status);
        }
    });

    const int result = run_until_finished(runtime, o);
    const auto stats = slave.statistics();
    log(o.participant, "responses=%llu ignored_headers=%llu frames=%llu wakeups=%llu overflows=%llu",
        static_cast<unsigned long long>(stats.responses_sent), static_cast<unsigned long long>(stats.headers_ignored),
        static_cast<unsigned long long>(stats.frames_received), static_cast<unsigned long long>(stats.wakeups_sent),
        static_cast<unsigned long long>(stats.inbox_overflows));
    if (result == kExitOk && stats.inbox_overflows != 0)
    {
        return kExitFailed;
    }
    return result;
}

} // namespace

int main(int argc, char** argv)
{
    return run_main(argc, argv, "lin-slave", print_usage, run);
}
