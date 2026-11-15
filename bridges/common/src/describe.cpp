// SPDX-License-Identifier: MIT
#include "vehicle_sil/bridge_runtime/describe.hpp"

#include "vehicle_sil/json/json_writer.hpp"

namespace vehicle_sil::bridge_runtime {

namespace {

/// The version of the JSON form (vehicle_sil::kEcuDescriptionFormat in vehicle_sil/ecu/describe.hpp).
/// The bridges do not link vehicle_sil::ecu; tests/bridge_runtime compares this output with
/// vehicle_sil::to_json() and reads it with vehicle_sil_launch's reader.
constexpr int kDescriptionFormat = 1;

} // namespace

std::string describe_json(const BridgeDescription& description)
{
    const bool can = description.kind == BridgedNetworkKind::Can;
    json::JsonWriter w;
    w.begin_object();
    w.integer("vehicle_sil_ecu", kDescriptionFormat);
    w.string("vehicle_sil", VEHICLE_SIL_VERSION);
    w.string("name", description.name);
    w.null("step_ns");
    w.begin_array("networks");
    w.begin_object();
    w.string("name", description.network);
    w.string("kind", can ? "can" : "ethernet");
    w.string("use", can ? "raw" : "ethernet");
    w.begin_array("nodes");
    w.end_array();
    w.null("mac");
    w.end_object();
    w.end_array();
    w.begin_array("variables");
    w.end_array();
    w.null("control");
    w.begin_array("features");
    w.end_array();
    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

} // namespace vehicle_sil::bridge_runtime
