include_guard(GLOBAL)

# Test executables of the in-process harness (tests/unit/test_harness.hpp): one CTest test per
# TEST_CASE(name) in SOURCES, named <PREFIX>.<name>, each in its own process (a failure or a
# crash stays in its test, and CTest runs the tests in parallel). The test names are read from the sources at configure time, so a
# new TEST_CASE is registered without editing a list.
#
#   vehicle_sil_add_harness_tests(<target>
#     PREFIX <prefix> LABELS <label>... TIMEOUT <seconds>
#     SOURCES <file>... LINK <target>...
#     [RUN_SERIAL <regex>])   # tests whose name matches run alone (wall-clock sensitive)
function(vehicle_sil_add_harness_tests target)
  cmake_parse_arguments(ARG "" "PREFIX;TIMEOUT;RUN_SERIAL" "LABELS;SOURCES;LINK" ${ARGN})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE vehicle_sil_test_main ${ARG_LINK})
  vehicle_sil_target_defaults(${target})
  vehicle_sil_copy_runtime_dlls(${target})

  set(names "")
  foreach(source IN LISTS ARG_SOURCES)
    get_filename_component(path "${source}" ABSOLUTE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(STRINGS "${path}" cases REGEX "^[ \t]*TEST_CASE[ \t]*\\([ \t]*[A-Za-z0-9_]+[ \t]*\\)")
    foreach(line IN LISTS cases)
      string(REGEX REPLACE "^[ \t]*TEST_CASE[ \t]*\\([ \t]*([A-Za-z0-9_]+)[ \t]*\\).*" "\\1" name "${line}")
      list(APPEND names ${name})
      set(test "${ARG_PREFIX}.${name}")
      add_test(NAME ${test} COMMAND ${target} ${name})
      # 77: the test cannot run on this machine (vehicle_sil::unit::Skipped).
      set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${ARG_TIMEOUT} SKIP_RETURN_CODE 77)
      if(ARG_RUN_SERIAL AND name MATCHES "${ARG_RUN_SERIAL}")
        set_tests_properties(${test} PROPERTIES RUN_SERIAL TRUE)
      endif()
    endforeach()
  endforeach()

  # Guards the scan above: every test case in the binary must have been registered.
  # Several targets may share a prefix: the first one's check is <PREFIX>.registration, the
  # others' <PREFIX>.registration.<target>.
  list(JOIN names "|" expected)
  set(registration ${ARG_PREFIX}.registration)
  if(TEST ${registration})
    set(registration ${ARG_PREFIX}.registration.${target})
  endif()
  add_test(NAME ${registration}
           COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:${target}> "-DEXPECTED=${expected}"
                   "-DEMULATOR=${CMAKE_CROSSCOMPILING_EMULATOR}"
                   -P ${PROJECT_SOURCE_DIR}/cmake/VehicleSilCheckTestRegistration.cmake)
  set_tests_properties(${registration} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT 60)
endfunction()

# Scenario tests of a process launcher: one CTest test <PREFIX>.<scenario> per scenario, running
#   <launcher> <ARGS>... --work-dir <WORK_DIR>/<scenario> --scenario <scenario>
#
#   vehicle_sil_add_scenario_tests(<launcher>
#     PREFIX <prefix> LABELS <label>... TIMEOUT <seconds> WORK_DIR <dir>
#     SCENARIOS <name>... ARGS <arg>... [RUN_SERIAL])
function(vehicle_sil_add_scenario_tests launcher)
  cmake_parse_arguments(ARG "RUN_SERIAL" "PREFIX;TIMEOUT;WORK_DIR" "LABELS;SCENARIOS;ARGS" ${ARGN})
  foreach(scenario IN LISTS ARG_SCENARIOS)
    set(test "${ARG_PREFIX}.${scenario}")
    add_test(NAME ${test}
             COMMAND ${launcher} ${ARG_ARGS} --work-dir "${ARG_WORK_DIR}/${scenario}" --scenario ${scenario})
    # 77: the scenario cannot run on this machine (it says why).
    set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${ARG_TIMEOUT} RUN_SERIAL ${ARG_RUN_SERIAL}
                                            SKIP_RETURN_CODE 77)
  endforeach()
endfunction()
