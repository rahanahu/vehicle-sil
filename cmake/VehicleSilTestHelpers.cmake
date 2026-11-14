include_guard(GLOBAL)

# The tests' upper bounds of waiting are multiplied by a factor when they run
# (tests/unit/test_timeout_scale.hpp): the environment variable VEHICLE_SIL_TEST_TIMEOUT_SCALE
# where they run, or, when it is not set, the build's default below (the tests get it as a compile
# definition, tests/CMakeLists.txt). The CTest TIMEOUT of the harness tests and the scenario tests
# below grows with the factor of configure time (the variable if set, else the default; rounded up
# to a whole factor, at least 1), so that CTest does not end a test before the test reports its own
# failure. The tests get that whole factor as VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR and say when
# the factor they run with is larger.
set(VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT "1" CACHE STRING
    "The factor on the tests' upper bounds of waiting when VEHICLE_SIL_TEST_TIMEOUT_SCALE is not set (digits, such as 4 or 2.5)")

# <out_factor> = <text> rounded up to a whole factor, at least 1, and <out_ok> = TRUE when <text>
# is digits with at most one point and greater than 0 (such as 3, 2.5 or .5): the rule of the
# other readers (tests/unit/test_timeout_scale.hpp, gui/server/tests/conftest.py,
# gui/web/e2e/support/timeouts.ts). Otherwise <out_ok> = FALSE and <out_factor> = 1.
function(_vehicle_sil_whole_timeout_factor text out_factor out_ok)
  set(${out_factor} 1 PARENT_SCOPE)
  set(${out_ok} FALSE PARENT_SCOPE)
  if(NOT text MATCHES "^[0-9]*\\.?[0-9]*$" OR NOT text MATCHES "[1-9]")
    return()
  endif()
  # Whole part and fraction (string(FIND): a "^" in string(REGEX REPLACE) also matches after a replacement).
  string(FIND "${text}" "." dot)
  if(dot LESS 0)
    set(whole "${text}")
    set(fraction "")
  else()
    string(SUBSTRING "${text}" 0 ${dot} whole)
    math(EXPR dot "${dot} + 1")
    string(SUBSTRING "${text}" ${dot} -1 fraction)
  endif()
  string(REGEX REPLACE "^0+" "" whole "${whole}")
  if(whole STREQUAL "")
    set(whole 0)
  endif()
  if(fraction MATCHES "[1-9]")
    math(EXPR whole "${whole} + 1")
  endif()
  if(whole LESS 1)
    set(whole 1)
  endif()
  set(${out_factor} ${whole} PARENT_SCOPE)
  set(${out_ok} TRUE PARENT_SCOPE)
endfunction()

_vehicle_sil_whole_timeout_factor("${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}" _vehicle_sil_timeout_factor _vehicle_sil_timeout_ok)
if(NOT _vehicle_sil_timeout_ok)
  message(FATAL_ERROR "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=\"${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}\" is not a number "
                      "greater than 0 in digits (such as 3 or 2.5): set it to such a factor (-DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4), "
                      "or to 1 for none")
endif()
set(_vehicle_sil_timeout_scale "$ENV{VEHICLE_SIL_TEST_TIMEOUT_SCALE}")
if(NOT _vehicle_sil_timeout_scale STREQUAL "")
  _vehicle_sil_whole_timeout_factor("${_vehicle_sil_timeout_scale}" _vehicle_sil_timeout_env_factor _vehicle_sil_timeout_ok)
  if(_vehicle_sil_timeout_ok)
    set(_vehicle_sil_timeout_factor ${_vehicle_sil_timeout_env_factor})
  else()
    message(WARNING "VEHICLE_SIL_TEST_TIMEOUT_SCALE=\"${_vehicle_sil_timeout_scale}\" is not a number greater than 0 in digits "
                    "(such as 3 or 2.5): the CTest timeouts are made for the build's default "
                    "(VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=${VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT}), and the tests will "
                    "refuse the value. Set it to a factor such as 3, or unset it.")
  endif()
endif()
set_property(GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR ${_vehicle_sil_timeout_factor})

# <out> = <seconds> times the factor above.
function(_vehicle_sil_scaled_ctest_timeout out seconds)
  get_property(factor GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR)
  math(EXPR scaled "${seconds} * ${factor}")
  set(${out} ${scaled} PARENT_SCOPE)
endfunction()

# The test <test> learns the whole factor its TIMEOUT was made for (VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR).
function(_vehicle_sil_tell_ctest_timeout_factor test)
  get_property(factor GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR)
  set_property(TEST ${test} APPEND PROPERTY ENVIRONMENT "VEHICLE_SIL_TEST_CTEST_TIMEOUT_FACTOR=${factor}")
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
      _vehicle_sil_tell_ctest_timeout_factor(${test})
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
#     SCENARIOS <name>... ARGS <arg>... [RUN_SERIAL] [MACHINE_LOCK <name>])
#
# MACHINE_LOCK: for scenarios that need something of which a machine has one, such as a fixed port
# that a document shows. Each test first takes the lock file /tmp/vehicle-sil-test-<name>.lock
# (flock, through cmake/vehicle_sil_machine_lock.sh), so that the builds of several working trees
# on one machine run such tests one at a time; RUN_SERIAL or RESOURCE_LOCK would hold only within
# one ctest. While it waits, the test says so, and the wait counts toward its TIMEOUT. Not on
# Windows; without flock, or when the file cannot be opened (another user's), the tests run
# without the lock (the script says so).
function(vehicle_sil_add_scenario_tests launcher)
  cmake_parse_arguments(ARG "RUN_SERIAL" "PREFIX;TIMEOUT;WORK_DIR;MACHINE_LOCK" "LABELS;SCENARIOS;ARGS" ${ARGN})
  _vehicle_sil_scaled_ctest_timeout(timeout ${ARG_TIMEOUT})
  set(lock "")
  if(ARG_MACHINE_LOCK AND NOT WIN32)
    find_program(VEHICLE_SIL_FLOCK flock)
    if(VEHICLE_SIL_FLOCK)
      set(lock /bin/sh "${PROJECT_SOURCE_DIR}/cmake/vehicle_sil_machine_lock.sh"
               "/tmp/vehicle-sil-test-${ARG_MACHINE_LOCK}.lock" "${ARG_MACHINE_LOCK}")
    else()
      message(STATUS "vehicle-sil: no flock, so ${ARG_PREFIX}.{${ARG_SCENARIOS}} run without the lock of the machine: "
                     "the same tests of another build on this machine may run at the same time")
    endif()
  endif()
  foreach(scenario IN LISTS ARG_SCENARIOS)
    set(test "${ARG_PREFIX}.${scenario}")
    add_test(NAME ${test}
             COMMAND ${lock} $<TARGET_FILE:${launcher}> ${ARG_ARGS} --work-dir "${ARG_WORK_DIR}/${scenario}"
                     --scenario ${scenario})
    # 77: the scenario cannot run on this machine (it says why).
    set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${timeout} RUN_SERIAL ${ARG_RUN_SERIAL}
                                            SKIP_RETURN_CODE 77)
    _vehicle_sil_tell_ctest_timeout_factor(${test})
  endforeach()
endfunction()
