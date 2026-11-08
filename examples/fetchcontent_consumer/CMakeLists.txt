# Example: consuming vehicle-sil from another CMake project with FetchContent.
#
#   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ./build/vehicle_sil_consumer
#
# SIL Kit comes in as a git submodule of vehicle-sil; FetchContent initialises it
# recursively, so nothing else has to be configured here.
cmake_minimum_required(VERSION 3.21)
project(vehicle_sil_fetchcontent_consumer LANGUAGES C CXX)

include(FetchContent)

set(VEHICLE_SIL_GIT_REPOSITORY "https://github.com/rahanahu/vehicle-sil.git"
    CACHE STRING "vehicle-sil git repository")
# Pin a full commit hash. Override with -DVEHICLE_SIL_GIT_TAG=<commit>.
set(VEHICLE_SIL_GIT_TAG "main" CACHE STRING "vehicle-sil commit to use")

FetchContent_Declare(
  vehicle_sil
  GIT_REPOSITORY ${VEHICLE_SIL_GIT_REPOSITORY}
  GIT_TAG ${VEHICLE_SIL_GIT_TAG})
FetchContent_MakeAvailable(vehicle_sil)

add_executable(vehicle_sil_consumer main.cpp)
# Link only what is needed: vehicle_sil::peer brings the J1939 stack, the CAN base, the SIL Kit
# port and vehicle_sil::core with it; the J1939 parts are targets of their own.
target_link_libraries(vehicle_sil_consumer PRIVATE vehicle_sil::peer vehicle_sil::j1939_periodic_sender
                      vehicle_sil::j1939_monitor)
# Windows only: copies SilKit.dll next to the executable.
vehicle_sil_copy_runtime_dlls(vehicle_sil_consumer)
