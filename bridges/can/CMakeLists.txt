# The part of a CAN bridge that does not depend on the external side.
#
#   vehicle_sil::can_bridge_core  CanBridge (SIL Kit's ICanController <-> an ExternalCanPort), the
#                                 statistics and health lines, ExternalPortAdapter (an
#                                 ExternalCanPort as a port of the common CAN foundation).
#                                 Platform independent, built everywhere. The Vector XL bridge
#                                 (bridges/vector-xl) implements ExternalCanPort for its channel.
add_library(vehicle_sil_can_bridge_core STATIC
  src/can_bridge.cpp
  src/external_port_adapter.cpp
)
add_library(vehicle_sil::can_bridge_core ALIAS vehicle_sil_can_bridge_core)
set_target_properties(vehicle_sil_can_bridge_core PROPERTIES EXPORT_NAME can_bridge_core)
target_include_directories(vehicle_sil_can_bridge_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_can_bridge_core
  PUBLIC SilKit::SilKit vehicle_sil::can vehicle_sil::can_silkit vehicle_sil::bridge_runtime PRIVATE vehicle_sil::json)
vehicle_sil_target_defaults(vehicle_sil_can_bridge_core)
