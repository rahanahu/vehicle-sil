include_guard(GLOBAL)

# The tests' upper bounds of waiting are multiplied by VEHICLE_SIL_TEST_TIMEOUT_SCALE when they run
# (tests/unit/test_timeout_scale.hpp). The CTest TIMEOUT of the harness tests and the scenario
# tests below grows with the value that the variable has at configure time (rounded up to a whole
# factor, at least 1), so that CTest does not end a test before the test reports its own failure.
set(_vehicle_sil_timeout_factor 1)
set(_vehicle_sil_timeout_scale "$ENV{VEHICLE_SIL_TEST_TIMEOUT_SCALE}")
if(_vehicle_sil_timeout_scale MATCHES "^[0-9]*\\.?[0-9]*$" AND _vehicle_sil_timeout_scale MATCHES "[0-9]")
  # Whole part and fraction (string(FIND): a "^" in string(REGEX REPLACE) also matches after a replacement).
  string(FIND "${_vehicle_sil_timeout_scale}" "." _vehicle_sil_timeout_dot)
  if(_vehicle_sil_timeout_dot LESS 0)
    set(_vehicle_sil_timeout_factor "${_vehicle_sil_timeout_scale}")
    set(_vehicle_sil_timeout_fraction "")
  else()
    string(SUBSTRING "${_vehicle_sil_timeout_scale}" 0 ${_vehicle_sil_timeout_dot} _vehicle_sil_timeout_factor)
    math(EXPR _vehicle_sil_timeout_dot "${_vehicle_sil_timeout_dot} + 1")
    string(SUBSTRING "${_vehicle_sil_timeout_scale}" ${_vehicle_sil_timeout_dot} -1 _vehicle_sil_timeout_fraction)
  endif()
  string(REGEX REPLACE "^0+" "" _vehicle_sil_timeout_factor "${_vehicle_sil_timeout_factor}")
  if(_vehicle_sil_timeout_factor STREQUAL "")
    set(_vehicle_sil_timeout_factor 0)
  endif()
  if(_vehicle_sil_timeout_fraction MATCHES "[1-9]")
    math(EXPR _vehicle_sil_timeout_factor "${_vehicle_sil_timeout_factor} + 1")
  endif()
  if(_vehicle_sil_timeout_factor LESS 1)
    set(_vehicle_sil_timeout_factor 1)
  endif()
elseif(NOT _vehicle_sil_timeout_scale STREQUAL "")
  message(WARNING "VEHICLE_SIL_TEST_TIMEOUT_SCALE=\"${_vehicle_sil_timeout_scale}\" is not a decimal number such as 3 "
                  "or 0.5: the CTest timeouts stay as they are. Write the factor in digits, or unset the variable.")
endif()
set_property(GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR ${_vehicle_sil_timeout_factor})

# <out> = <seconds> times the factor above.
function(_vehicle_sil_scaled_ctest_timeout out seconds)
  get_property(factor GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR)
  math(EXPR scaled "${seconds} * ${factor}")
  set(${out} ${scaled} PARENT_SCOPE)
endfunction()

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
  _vehicle_sil_scaled_ctest_timeout(timeout ${ARG_TIMEOUT})
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
      set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${timeout} SKIP_RETURN_CODE 77)
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
  _vehicle_sil_scaled_ctest_timeout(timeout ${ARG_TIMEOUT})
  foreach(scenario IN LISTS ARG_SCENARIOS)
    set(test "${ARG_PREFIX}.${scenario}")
    add_test(NAME ${test}
             COMMAND ${launcher} ${ARG_ARGS} --work-dir "${ARG_WORK_DIR}/${scenario}" --scenario ${scenario})
    # 77: the scenario cannot run on this machine (it says why).
    set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${timeout} RUN_SERIAL ${ARG_RUN_SERIAL}
                                            SKIP_RETURN_CODE 77)
  endforeach()
endfunction()
