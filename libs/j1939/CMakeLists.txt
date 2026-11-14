# vehicle_sil::j1939 - the J1939 stack (namespace vehicle_sil::j1939, headers
# vehicle_sil/j1939/). Runs on vehicle_sil::can only: no SIL Kit, no Vector XL.
add_library(vehicle_sil_j1939 STATIC
  src/network.cpp
  src/node.cpp
  src/timer_set.cpp
  src/toolbox.cpp
  src/transport.cpp
  src/types.cpp
)
add_library(vehicle_sil::j1939 ALIAS vehicle_sil_j1939)
set_target_properties(vehicle_sil_j1939 PROPERTIES EXPORT_NAME j1939)
target_include_directories(vehicle_sil_j1939 PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_j1939 PUBLIC vehicle_sil::can)
vehicle_sil_target_defaults(vehicle_sil_j1939)

# Protocol parts: one target each, built on the public API and the toolbox above only
# (checked by the test j1939_parts.boundary). In the order of the names; the order has no meaning.
add_subdirectory(parts/address_commander)
add_subdirectory(parts/broadcast_suppression)
add_subdirectory(parts/diagnostics)
add_subdirectory(parts/exchange)
add_subdirectory(parts/monitor)
add_subdirectory(parts/periodic_sender)
add_subdirectory(parts/requester)

# Typed messages (header only, on the engine's value types; codec_node.hpp adapts them to Node
# and PeriodicSender).
add_subdirectory(codec)
