# vehicle_sil::spy on a LocalBench (public API only), its allocation in the steps (a binary of its
# own: it replaces the global operator new) and its boundary. The prefix j1939_spy puts the tests
# among those of the J1939 stack ('^j1939').
# Spy::watch<M>() takes the generated types of tests/j1939 (vehicle_sil_codec_test_messages), types
# written by hand (hand_written_messages.hpp) and those of the guide's example (cal_messages.yaml).
vehicle_sil_add_j1939_codec(vehicle_sil_spy_cal_messages cal_messages.yaml)
vehicle_sil_add_harness_tests(vehicle_sil_spy_tests
  PREFIX j1939_spy LABELS unit j1939 TIMEOUT 60
  SOURCES test_spy.cpp test_spy_watch.cpp test_spy_analysis.cpp
  LINK vehicle_sil::spy vehicle_sil::local vehicle_sil::simulation vehicle_sil_codec_test_messages
       vehicle_sil_spy_cal_messages)
target_include_directories(vehicle_sil_spy_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/tests/j1939)

# A signal named like an item of the watch ("count") is a build error that says so: the test builds
# a target that is not part of the build (cmake --build of this build tree, with the build tool)
# and expects the message of the static_assert. Registered only where that build works: not when
# vehicle-sil is a subproject (the build tree is the parent's) and not with a generator of several
# configurations (the test would build another configuration than the one ctest runs).
add_library(vehicle_sil_spy_watch_clash OBJECT EXCLUDE_FROM_ALL watch_clash.cpp)
target_link_libraries(vehicle_sil_spy_watch_clash PRIVATE vehicle_sil::spy vehicle_sil::j1939_codec)
target_include_directories(vehicle_sil_spy_watch_clash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
get_property(_vehicle_sil_spy_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT vehicle_sil_IS_TOP_LEVEL)
  message(STATUS "vehicle-sil: the test j1939_spy.watch_of_a_signal_named_count_does_not_build is left out: it builds in "
                 "vehicle-sil's own build tree, and here vehicle-sil is a subproject")
elseif(_vehicle_sil_spy_multi_config)
  message(STATUS "vehicle-sil: the test j1939_spy.watch_of_a_signal_named_count_does_not_build is left out: the generator "
                 "${CMAKE_GENERATOR} has several configurations; use a single-configuration generator (Ninja) to run it")
else()
  _vehicle_sil_scaled_ctest_timeout(_vehicle_sil_spy_clash_timeout 300)
  add_test(NAME j1939_spy.watch_of_a_signal_named_count_does_not_build
           COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target vehicle_sil_spy_watch_clash)
  set_tests_properties(j1939_spy.watch_of_a_signal_named_count_does_not_build PROPERTIES
    LABELS "unit;j1939" TIMEOUT ${_vehicle_sil_spy_clash_timeout}
    PASS_REGULAR_EXPRESSION "a signal of M is named like an item the watch publishes")
endif()

# The examples of the guide are guide_example.hpp, guide_analysis.hpp and cal_messages.yaml (the
# English page; the Japanese page has the same blocks, which scripts/check-docs.py checks).
add_test(NAME j1939_spy.guide_example_is_the_guides
         COMMAND ${CMAKE_COMMAND} -DDOC=${VEHICLE_SIL_TESTED_DOCS}/j1939-guide.md
                 -DEXAMPLE=${CMAKE_CURRENT_SOURCE_DIR}/guide_example.hpp
                 -DANALYSIS=${CMAKE_CURRENT_SOURCE_DIR}/guide_analysis.hpp
                 -DMESSAGES=${CMAKE_CURRENT_SOURCE_DIR}/cal_messages.yaml
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/guide_example.cmake)
set_tests_properties(j1939_spy.guide_example_is_the_guides PROPERTIES LABELS "unit;j1939" TIMEOUT 60)

# Replaces the global operator new to count allocations in steady-state steps: its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_spy_allocation_tests
  PREFIX j1939_spy LABELS unit j1939 TIMEOUT 60
  SOURCES test_spy_allocation.cpp
  LINK vehicle_sil::spy vehicle_sil::local vehicle_sil::j1939_codec)
target_include_directories(vehicle_sil_spy_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                           ${PROJECT_SOURCE_DIR}/tests/j1939)

# libs/spy uses the public headers of the engine and of Ecu only, like the parts.
add_test(NAME j1939_spy.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/spy -DSOURCES_ONLY=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_spy.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
