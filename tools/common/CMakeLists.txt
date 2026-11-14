# Header-only helpers shared by the vehicle-sil executables (tools, examples, test launchers):
# command-line parsing, the command line as UTF-8 and process exit codes. Internal; not part of
# the library API.
add_library(vehicle_sil_cli INTERFACE)
target_include_directories(vehicle_sil_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(vehicle_sil_cli INTERFACE cxx_std_20)
if(WIN32)
  # CommandLineToArgvW (vehicle_sil/cli/utf8.hpp).
  target_link_libraries(vehicle_sil_cli INTERFACE shell32)
endif()
