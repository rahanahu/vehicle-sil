# SPDX-License-Identifier: MIT-0
# Your ECUs on vehicle-sil: two ECU executables (engine/, dashboard/), the simulation of the two
# (simulation.yaml) and a scenario for it (scenarios/). README.md has the steps.
#
#   cmake --preset linux             Windows, in the MSYS2 UCRT64 shell: cmake --preset windows
#   cmake --build --preset linux                                         cmake --build --preset windows
#
# The executables go to bin/, where simulation.yaml finds them. The first configure fetches
# vehicle-sil and SIL Kit, and the first build compiles SIL Kit (a few minutes, once).
cmake_minimum_required(VERSION 3.21)
project(my_ecus LANGUAGES C CXX)

# vehicle-sil's own tests build this directory inside vehicle-sil, which is there already.
if(NOT TARGET vehicle_sil::simulation)
  include(FetchContent)
  FetchContent_Declare(
    vehicle_sil
    GIT_REPOSITORY https://github.com/rahanahu/vehicle-sil.git
    # The vehicle-sil release your ECUs are built with. Keep it the same as the version of the
    # tools (vehicle_sil_launch version): change both together.
    GIT_TAG v0.1.0
    GIT_SHALLOW TRUE)
  FetchContent_MakeAvailable(vehicle_sil)
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${PROJECT_SOURCE_DIR}/bin)
endif()

# One executable per ECU. vehicle_sil::simulation brings ecu_main() and everything an ECU uses
# (the J1939 stack, CAN, LIN, the variables); link the J1939 parts you use as well.
add_executable(engine_ecu engine/main.cpp)
target_include_directories(engine_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(engine_ecu PRIVATE vehicle_sil::simulation)
# Windows only: copies SilKit.dll next to the executable.
vehicle_sil_copy_runtime_dlls(engine_ecu)

add_executable(dashboard_ecu dashboard/main.cpp)
target_include_directories(dashboard_ecu PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(dashboard_ecu PRIVATE vehicle_sil::simulation)
vehicle_sil_copy_runtime_dlls(dashboard_ecu)
