# Header-only helpers shared by the vehicle-sil executables (tools, examples, test launchers):
# command-line parsing and process exit codes. Internal; not part of the library API.
add_library(vehicle_sil_cli INTERFACE)
target_include_directories(vehicle_sil_cli INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(vehicle_sil_cli INTERFACE cxx_std_20)
