# SocketCAN CAN bridge (Linux only: a physical CAN adapter or a virtual one, vcan).
#
#   vehicle_sil::socketcan_bridge_core  the bridge without the kernel's own calls: the command line,
#                                       the CAN interfaces (netlink answers, the checks of an
#                                       interface, the lists), the error frames, and SocketCanPort,
#                                       an ExternalCanPort written against the interface CanDevice
#                                       (can_device.hpp). The tests link it with a fake CanDevice.
#   vehicle_sil_socketcan_bridge        the executable, with LinuxCanDevice (src/linux_can_device.cpp),
#                                       which calls the kernel.
#
# Nothing here is built on another OS.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()
find_package(Threads REQUIRED)

add_library(vehicle_sil_socketcan_bridge_core STATIC
  src/bridge_options.cpp
  src/can_interfaces.cpp
  src/error_frame.cpp
  src/netlink.cpp
  src/socketcan_port.cpp
)
add_library(vehicle_sil::socketcan_bridge_core ALIAS vehicle_sil_socketcan_bridge_core)
set_target_properties(vehicle_sil_socketcan_bridge_core PROPERTIES EXPORT_NAME socketcan_bridge_core)
target_include_directories(vehicle_sil_socketcan_bridge_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_socketcan_bridge_core
  PUBLIC SilKit::SilKit vehicle_sil::can_bridge_core vehicle_sil::bridge_runtime Threads::Threads PRIVATE vehicle_sil::json)
vehicle_sil_target_defaults(vehicle_sil_socketcan_bridge_core)

add_executable(vehicle_sil_socketcan_bridge app/main.cpp src/linux_can_device.cpp)
target_link_libraries(vehicle_sil_socketcan_bridge
  PRIVATE vehicle_sil::socketcan_bridge_core vehicle_sil::bridge_runtime vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_socketcan_bridge)
