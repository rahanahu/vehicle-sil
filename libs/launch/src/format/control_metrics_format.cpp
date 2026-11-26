// SPDX-License-Identifier: MIT
#include "vehicle_sil/launch/control_metrics_format.hpp"

#include "vehicle_sil/ecu/describe.hpp"
#include "vehicle_sil/json/json_writer.hpp"

#include <algorithm>
#include <cstdint>

namespace vehicle_sil::launch {
namespace {

using vehicle_sil::json::JsonWriter;

const char* simulation_name(runner::SimulationEnd end)
{
    switch (end)
    {
    case runner::SimulationEnd::NotStarted: return "not_started";
    case runner::SimulationEnd::ReachedStopAt: return "reached_stop_at";
    case runner::SimulationEnd::Ended: return "ended";
    case runner::SimulationEnd::Aborted: return "aborted";
    case runner::SimulationEnd::Interrupted: return "interrupted";
    }
    return "not_started";
}

const char* result_name(RunResult result)
{
    switch (result)
    {
    case RunResult::Passed: return "passed";
    case RunResult::Stopped: return "stopped";
    case RunResult::Incomplete: return "incomplete";
    case RunResult::Failed: return "failed";
    case RunResult::Interrupted: return "interrupted";
    case RunResult::CheckFailed: return "check_failed";
    case RunResult::Unreadable: return "unreadable";
    case RunResult::SetupFailed: return "setup_failed";
    case RunResult::Usage: return "usage";
    }
    return "failed";
}

const char* unavailable_reason(const RunReport& report)
{
    if (!report.check.config)
    {
        return "unreadable";
    }
    if (!report.run)
    {
        return "check_failed";
    }
    if (report.run->close.not_closed_ms)
    {
        return "internal_error";
    }
    switch (runner::end_reason(*report.run))
    {
    case runner::EndReason::Stopped: return "stopped_before_setup";
    case runner::EndReason::Interrupted: return "interrupted_before_setup";
    case runner::EndReason::SetupFailed: return "setup_failed";
    case runner::EndReason::Completed:
    case runner::EndReason::Failed: return "internal_error";
    }
    return "internal_error";
}

void write_text_or_null(JsonWriter& w, std::string_view key, std::string_view value)
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

void write_source(JsonWriter& w, bool available)
{
    w.begin_object("source");
    w.string("transport", "silkit");
    w.string("collector", "HubTransport::stats");
    w.string("scope", "cumulative_hub_transport_lifetime");
    if (available)
    {
        w.string("collection_point", "before_transport_close");
    }
    else
    {
        w.null("collection_point");
    }
    w.end_object();
}

} // namespace

std::string format_control_metrics_json(const RunReport& report)
{
    const bool available = report.run && report.run->control.transport_stats.has_value();
    JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_control_metrics", kControlMetricsFormat);
    w.string("vehicle_sil", version());
    write_text_or_null(w, "file", report.check.file);
    if (report.check.config && !report.check.config->variant.empty())
    {
        w.string("variant", report.check.config->variant);
    }
    else
    {
        w.null("variant");
    }
    w.string("status", available ? "available" : "unavailable");
    if (available)
    {
        w.null("unavailable_reason");
    }
    else
    {
        w.string("unavailable_reason", unavailable_reason(report));
    }
    write_source(w, available);

    w.begin_object("run");
    w.string("result", result_name(report.result()));
    if (report.run)
    {
        w.string("end", runner::end_reason_name(runner::end_reason(*report.run)));
        w.string("simulation", simulation_name(report.run->end.simulation));
    }
    else
    {
        w.null("end");
        w.null("simulation");
    }
    w.boolean("ok", report.ok());
    const bool has_timing = report.run && report.run->end.simulation != runner::SimulationEnd::NotStarted &&
                            report.run->start.before_start_error.empty();
    if (has_timing)
    {
        w.integer("sim_ms", report.run->timing.sim_ms);
        w.integer("wall_ms", report.run->timing.wall_ms);
    }
    else
    {
        w.null("sim_ms");
        w.null("wall_ms");
    }
    if (report.run && report.run->timing.time_step_ms > 0)
    {
        w.integer("time_step_ms", report.run->timing.time_step_ms);
    }
    else
    {
        w.null("time_step_ms");
    }
    if (report.run && !report.run->end.interrupted_by.empty())
    {
        w.string("interrupted_by", report.run->end.interrupted_by);
    }
    else
    {
        w.null("interrupted_by");
    }
    w.end_object();

    if (available)
    {
        const runner::ControlTransportStats& c = *report.run->control.transport_stats;
        w.begin_object("counters");
        w.unsigned_integer("messages", c.messages);
        w.unsigned_integer("bytes", c.bytes);
        w.unsigned_integer("handler_calls", c.handler_calls);
        w.integer("handler_time_ns", std::max<std::int64_t>(0, c.handler_time_ns));
        w.integer("max_handler_time_ns", std::max<std::int64_t>(0, c.max_handler_time_ns));
        w.unsigned_integer("hellos", c.hellos);
        w.unsigned_integer("last_words", c.last_words);
        w.unsigned_integer("late_messages", c.late_messages);
        w.unsigned_integer("undecodable", c.undecodable);
        w.unsigned_integer("unknown", c.unknown);
        w.unsigned_integer("refused_hellos", c.refused_hellos);
        w.end_object();
    }
    else
    {
        w.null("counters");
    }
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

std::string format_control_metrics_internal_error_json(std::string_view file, const std::optional<std::string>& variant)
{
    JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_control_metrics", kControlMetricsFormat);
    w.string("vehicle_sil", version());
    write_text_or_null(w, "file", file);
    if (variant && !variant->empty())
    {
        w.string("variant", *variant);
    }
    else
    {
        w.null("variant");
    }
    w.string("status", "unavailable");
    w.string("unavailable_reason", "internal_error");
    write_source(w, false);

    w.begin_object("run");
    w.null("result");
    w.null("end");
    w.null("simulation");
    w.boolean("ok", false);
    w.null("sim_ms");
    w.null("wall_ms");
    w.null("time_step_ms");
    w.null("interrupted_by");
    w.end_object();

    w.null("counters");
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil::launch
