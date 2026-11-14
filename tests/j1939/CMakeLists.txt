# The J1939 stack (vehicle_sil::j1939) on the virtual bus. Links no SIL Kit, so
# building it checks that the stack needs none. Uses no example: added with or without
# VEHICLE_SIL_BUILD_EXAMPLES.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_tests
  PREFIX j1939 LABELS unit j1939 TIMEOUT 60
  SOURCES
    test_j1939_address_claim.cpp
    test_j1939_address_management.cpp
    test_j1939_bus_timing.cpp
    test_j1939_network.cpp
    test_j1939_observer.cpp
    test_j1939_outage.cpp
    test_j1939_real_time.cpp
    test_j1939_request.cpp
    test_j1939_toolbox.cpp
    test_j1939_transport_bam.cpp
    test_j1939_transport_faults.cpp
    test_j1939_transport_rts.cpp
    test_j1939_types.cpp
  LINK vehicle_sil::j1939)
target_include_directories(vehicle_sil_j1939_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The protocol parts (DM13, diagnostics, identification, periodic sender, requester, monitor,
# address commander): built only from the engine's public API
# and toolbox; the boundary test checks their sources and links.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_parts_tests
  PREFIX j1939_parts LABELS unit j1939 TIMEOUT 60
  SOURCES
    test_j1939_diagnostics.cpp
    test_j1939_dm13.cpp
    test_j1939_exchange.cpp
    test_j1939_identification.cpp
    test_j1939_monitor.cpp
    test_j1939_periodic_sender.cpp
    test_j1939_pid_stream_example.cpp
    test_j1939_requester.cpp
  LINK vehicle_sil::j1939_diagnostics vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester
       vehicle_sil::j1939_monitor vehicle_sil::j1939_address_commander vehicle_sil::j1939_exchange)
# The part of examples/pid_stream is header only: its test includes it from there.
target_include_directories(vehicle_sil_j1939_parts_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                                                                  ${PROJECT_SOURCE_DIR}/examples/pid_stream)
add_test(NAME j1939_parts.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/j1939/parts
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_parts.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# The check itself: a hidden friend function passes, befriending a class or type fails.
foreach(fixture IN ITEMS hidden_friend friend_class friend_type)
  add_test(NAME j1939_parts.boundary_check_${fixture}
           COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/${fixture}
                   -DSOURCES_ONLY=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
  set_tests_properties(j1939_parts.boundary_check_${fixture} PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
endforeach()
# Fails for the right reason (the exit code is not used with a regular expression).
set_tests_properties(j1939_parts.boundary_check_friend_class j1939_parts.boundary_check_friend_type
                     PROPERTIES PASS_REGULAR_EXPRESSION "reach into the J1939 engine:.*part\\.hpp: matches 'friend")
add_test(NAME j1939_parts.boundary_check_observer
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/observer
                 -DSOURCES_ONLY=ON -DFORBID_OBSERVER=ON -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_parts.boundary_check_observer PROPERTIES LABELS "unit;j1939" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "reach into the J1939 engine:.*part\\.hpp: matches 'set_observer'")
# The local buses, the test kit, Peer, Ecu and Simulation use the engine's public API only, like
# the parts.
foreach(dir IN ITEMS local testkit peer ecu simulation)
  add_test(NAME ${dir}.boundary
           COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/${dir} -DSOURCES_ONLY=ON
                   -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
  set_tests_properties(${dir}.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
endforeach()

# Typed messages: the generator (Python, its error checks) and the generated types
# (codec/codec_test_messages.yaml), alone and on a node.
vehicle_sil_add_j1939_codec(vehicle_sil_codec_test_messages codec/codec_test_messages.yaml
                            codec/codec_tricky_names.yaml codec/codec_profiles.yaml)
vehicle_sil_add_harness_tests(vehicle_sil_j1939_codec_tests
  PREFIX j1939_codec LABELS unit j1939 TIMEOUT 60
  SOURCES
    test_j1939_codec.cpp
    test_j1939_codec_node.cpp
    test_j1939_codec_profiles.cpp
  LINK vehicle_sil_codec_test_messages vehicle_sil::j1939_monitor vehicle_sil::testkit)
target_include_directories(vehicle_sil_j1939_codec_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME j1939_codec.generator
         COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${PROJECT_SOURCE_DIR}/tools/j1939_codegen/test_j1939_codegen.py)
set_tests_properties(j1939_codec.generator PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# codec.hpp knows the engine's value types only; codec_node.hpp uses the public API only.
add_test(NAME j1939_codec.independent_of_the_engine
         COMMAND ${CMAKE_COMMAND} -DHEADER=${PROJECT_SOURCE_DIR}/libs/j1939/codec/include/vehicle_sil/j1939/codec.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Codec.cmake)
add_test(NAME j1939_codec.boundary
         COMMAND ${CMAKE_COMMAND} -DPARTS_DIR=${PROJECT_SOURCE_DIR}/libs/j1939/codec -DSOURCES_ONLY=ON
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Parts.cmake)
set_tests_properties(j1939_codec.independent_of_the_engine j1939_codec.boundary PROPERTIES LABELS "unit;j1939" TIMEOUT 60)
# GCC's -fsanitize=undefined makes some expressions no constant expressions (a pointer compared
# with nullptr): the codec's compile-time checks must still compile with it. Syntax only, so it
# needs no libubsan.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set(_codec_includes "$<TARGET_PROPERTY:vehicle_sil_codec_test_messages,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_j1939_codec,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_j1939,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_can,INTERFACE_INCLUDE_DIRECTORIES>"
                      "$<TARGET_PROPERTY:vehicle_sil_test_main,INTERFACE_INCLUDE_DIRECTORIES>"
                      "${CMAKE_CURRENT_SOURCE_DIR}")
  add_test(NAME j1939_codec.compiles_with_gcc_undefined_behavior_sanitizer
           COMMAND ${CMAKE_CXX_COMPILER} -std=c++20 -fsyntax-only -fsanitize=undefined -Wall -Wextra -Werror
                   "-I$<JOIN:${_codec_includes},;-I>"
                   ${CMAKE_CURRENT_SOURCE_DIR}/test_j1939_codec.cpp
                   ${CMAKE_CURRENT_SOURCE_DIR}/test_j1939_codec_profiles.cpp
           COMMAND_EXPAND_LISTS)
  set_tests_properties(j1939_codec.compiles_with_gcc_undefined_behavior_sanitizer PROPERTIES LABELS "unit;j1939" TIMEOUT 120)
endif()
# The check itself: a header that includes the engine (with angle brackets) fails.
add_test(NAME j1939_codec.independent_check_rejects_the_engine
         COMMAND ${CMAKE_COMMAND} -DHEADER=${CMAKE_CURRENT_SOURCE_DIR}/boundary_fixtures/codec_includes_node/codec.hpp
                 -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckJ1939Codec.cmake)
set_tests_properties(j1939_codec.independent_check_rejects_the_engine PROPERTIES LABELS "unit;j1939" TIMEOUT 60
                     PASS_REGULAR_EXPRESSION "found: vehicle_sil/j1939/network.hpp")

# Replaces the global operator new to count allocations in steady-state steps: its own binary.
vehicle_sil_add_harness_tests(vehicle_sil_j1939_allocation_tests
  PREFIX j1939 LABELS unit j1939 TIMEOUT 60
  SOURCES test_j1939_allocation.cpp
  LINK vehicle_sil::j1939 vehicle_sil::j1939_diagnostics vehicle_sil::j1939_periodic_sender vehicle_sil::j1939_requester
       vehicle_sil::j1939_monitor vehicle_sil::j1939_address_commander vehicle_sil::j1939_exchange
       vehicle_sil_codec_test_messages)
target_include_directories(vehicle_sil_j1939_allocation_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# The J1939 stack on SIL Kit participants (in-process registry): same traces as on the
# virtual bus, with SIL Kit's trivial simulation and with the simple CAN network simulator
# (vehicle_sil::can_netsim, which is not built without SIL Kit's rapidyaml).
if(TARGET vehicle_sil_can_netsim)
  vehicle_sil_add_harness_tests(vehicle_sil_j1939_silkit_tests
    PREFIX j1939_silkit LABELS unit j1939 TIMEOUT 180
    SOURCES test_j1939_silkit.cpp
    LINK vehicle_sil::j1939 vehicle_sil::can_silkit vehicle_sil::can_netsim)
endif()
