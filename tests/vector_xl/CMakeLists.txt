# Vector XL bridge tests. They run on every platform: the XL backend logic
# (bridges/vector-xl/src/xl_driver.cpp) is linked with the TEST-ONLY fake in fake_xl/, which
# implements the internal driver interface bridges/vector-xl/src/xl_backend.hpp, instead of
# the implementation that calls Vector's library.
set(_bridge_dir "${PROJECT_SOURCE_DIR}/bridges/vector-xl")

# The production XL backend logic plus the fake backend and its test control API.
add_library(vehicle_sil_vector_xl_on_fake STATIC fake_xl/fake_xl_driver.cpp)
target_include_directories(vehicle_sil_vector_xl_on_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/fake_xl ${_bridge_dir}/src)
target_link_libraries(vehicle_sil_vector_xl_on_fake
  PUBLIC vehicle_sil::vector_xl_bridge_core
  PRIVATE vehicle_sil_vector_xl_logic)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_on_fake)

# The production bridge executable, linked against the fake driver.
add_executable(vehicle_sil_vector_xl_bridge_on_fake ${_bridge_dir}/app/main.cpp)
target_link_libraries(vehicle_sil_vector_xl_bridge_on_fake PRIVATE vehicle_sil_vector_xl_on_fake vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_bridge_on_fake)
vehicle_sil_copy_runtime_dlls(vehicle_sil_vector_xl_bridge_on_fake)

# In-process tests, one CTest test per case (each in its own process).
vehicle_sil_add_harness_tests(vehicle_sil_vector_xl_tests
  PREFIX vector_xl LABELS vector_xl unit TIMEOUT 60
  SOURCES test_can_bridge.cpp test_bridge_options.cpp test_xl_port.cpp test_bridge_silkit.cpp
          test_external_port_adapter.cpp
  LINK vehicle_sil_vector_xl_on_fake vehicle_sil::core)

# Process level tests of the bridge executable.
add_executable(vehicle_sil_vector_xl_it_peer xl_bridge_it_peer.cpp)
target_link_libraries(vehicle_sil_vector_xl_it_peer PRIVATE vehicle_sil::core vehicle_sil_cli)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_it_peer)
vehicle_sil_copy_runtime_dlls(vehicle_sil_vector_xl_it_peer)

add_executable(vehicle_sil_vector_xl_it_launcher xl_bridge_it_launcher.cpp)
target_link_libraries(vehicle_sil_vector_xl_it_launcher PRIVATE vehicle_sil_test_process)
vehicle_sil_target_defaults(vehicle_sil_vector_xl_it_launcher)
add_dependencies(vehicle_sil_vector_xl_it_launcher
  sil-kit-registry vehicle_sil_vector_xl_bridge_on_fake vehicle_sil_vector_xl_it_peer)

vehicle_sil_add_scenario_tests(vehicle_sil_vector_xl_it_launcher
  PREFIX vector_xl.integration LABELS vector_xl integration TIMEOUT 120
  WORK_DIR ${CMAKE_CURRENT_BINARY_DIR}/integration
  SCENARIOS echo-roundtrip echo-roundtrip-receipts echo-roundtrip-physical list-channels bad-channel bad-usage no-driver
            device-lost
  ARGS --registry $<TARGET_FILE:sil-kit-registry>
       --bridge $<TARGET_FILE:vehicle_sil_vector_xl_bridge_on_fake>
       --peer $<TARGET_FILE:vehicle_sil_vector_xl_it_peer>
       --timeout-ms 60000)
