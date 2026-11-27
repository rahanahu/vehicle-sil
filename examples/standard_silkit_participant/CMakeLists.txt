# Standalone standard Vector SIL Kit C++ participant sample.
#
# Standalone build against an official Vector SIL Kit SDK (zero vehicle-sil dependencies):
#   cmake -S examples/standard_silkit_participant -B /tmp/build-std-peer -G Ninja \
#     -DCMAKE_BUILD_TYPE=Release \
#     -DCMAKE_PREFIX_PATH=/path/to/SilKit-5.0.8-.../SilKit
#   cmake --build /tmp/build-std-peer
#
# When included from the vehicle-sil top-level CMake build, the existing SilKit::SilKit target
# is reused so CI does not need to download an external SIL Kit SDK.
cmake_minimum_required(VERSION 3.21)
project(standard_silkit_participant LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT TARGET SilKit::SilKit)
  find_package(SilKit REQUIRED)
endif()

add_executable(standard_silkit_participant main.cpp rx_inbox.hpp)
target_link_libraries(standard_silkit_participant PRIVATE SilKit::SilKit)

# Inside a vehicle-sil top-level build on Windows, copy runtime DLLs beside the executable so
# launcher tests can run it without modifying PATH. Standalone builds skip this automatically.
if(COMMAND vehicle_sil_copy_runtime_dlls)
  vehicle_sil_copy_runtime_dlls(standard_silkit_participant)
endif()

# Copy the example launcher configuration next to the built executable when building out-of-source.
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_CURRENT_BINARY_DIR)
  configure_file(simulation.yaml simulation.yaml COPYONLY)
endif()
