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

# Opt-in batching of pure unit TEST_CASE entries per source file into fewer CTest processes
# (for CI, where sanitizer and Windows process startup dominates thousands of tiny unit tests).
# Kept OFF by default so local `ctest -R <prefix>.<case>` and per-case crash isolation stay unchanged.
option(VEHICLE_SIL_BATCH_UNIT_TESTS
       "Batch fast unit test cases per source file into fewer CTest processes (for CI)" OFF)

# <out_factor> = <text> rounded up to a whole factor, at least 1 and at most 1000000000 (1e9, matching
# whole_timeout_factor in tests/unit/test_timeout_scale.hpp), and <out_ok> = TRUE when <text>
# is digits with at most one point, finite in IEEE-754 double, and greater than 0 (such as 3, 2.5
# or .5): the rule of the other readers (tests/unit/test_timeout_scale.hpp,
# tests/common/timeout_scale.py, gui/server/tests/conftest.py, gui/web/e2e/support/timeouts.ts).
# Otherwise <out_ok> = FALSE and <out_factor> = 1.
function(_vehicle_sil_whole_timeout_factor text out_factor out_ok)
  set(${out_factor} 1 PARENT_SCOPE)
  set(${out_ok} FALSE PARENT_SCOPE)
  if(NOT text MATCHES "[1-9]" OR NOT text MATCHES "^0*([0-9]*)(\\.([0-9]*))?$")
    return()
  endif()
  set(whole "${CMAKE_MATCH_1}")
  set(fraction "${CMAKE_MATCH_3}")
  if(whole STREQUAL "")
    # Reject IEEE-754 double underflow to 0.0: values <= 2^-1075 (half DBL_TRUE_MIN, round-to-even).
    if(NOT fraction MATCHES "^(0*)([1-9][0-9]*)$")
      return()
    endif()
    string(LENGTH "${CMAKE_MATCH_1}" _lz)
    string(REGEX REPLACE "0+$" "" _sig "${CMAKE_MATCH_2}")
    set(_dbl_true_min_half_sig "24703282292062327208828439643411068618252990130716238221279284125033775363510437593264991818081799618989828234772285886546332835517796989819938739800539093906315035659515570226392290858392449105184435931802849936536152500319370457678249219365623669863658480757001585769269903706311928279558551332927834338409351978015531246597263579574622766465272827220056374006485499977096599470454020828166226237857393450736339007967761930577506740176324673600968951340535537458516661134223766678604162159680461914467291840300530057530849048765391711386591646239524912623653881879636239373280423891018672348497668235089863388587925628302755995657524455507255189313690836254779186948667994968324049705821028513185451396213837722826145437693412532098591327667236328125")
    if(_lz GREATER 323)
      return()
    endif()
    if(_lz EQUAL 323 AND NOT _sig STRGREATER "${_dbl_true_min_half_sig}")
      return()
    endif()
    set(${out_factor} 1 PARENT_SCOPE)
    set(${out_ok} TRUE PARENT_SCOPE)
    return()
  endif()
  # Reject IEEE-754 double overflow to +inf: values >= (2^54 - 1) * 2^970 (DBL_MAX + 0.5 ulp, round-to-even).
  string(LENGTH "${whole}" _whole_len)
  set(_dbl_max_cutoff "179769313486231580793728971405303415079934132710037826936173778980444968292764750946649017977587207096330286416692887910946555547851940402630657488671505820681908902000708383676273854845817711531764475730270069855571366959622842914819860834936475292719074168444365510704342711559699508093042880177904174497792")
  if(_whole_len GREATER 309)
    return()
  endif()
  if(_whole_len EQUAL 309 AND NOT whole STRLESS "${_dbl_max_cutoff}")
    return()
  endif()
  # Clamp to 1000000000 (1e9) before math(EXPR) to avoid 64-bit overflow or wraparound.
  if(_whole_len GREATER 10 OR (_whole_len EQUAL 10 AND NOT whole STRLESS "1000000000"))
    set(${out_factor} 1000000000 PARENT_SCOPE)
    set(${out_ok} TRUE PARENT_SCOPE)
    return()
  endif()
  if(fraction MATCHES "[1-9]")
    math(EXPR whole "${whole} + 1")
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
#     [RUN_SERIAL <regex>]    # tests whose name matches run alone (wall-clock sensitive)
#     [REGISTRATION <name>])  # the name of the check below, when one prefix is shared by targets of
#                             # several directories (a directory cannot see the tests of another)
function(vehicle_sil_add_harness_tests target)
  cmake_parse_arguments(ARG "" "PREFIX;TIMEOUT;RUN_SERIAL;REGISTRATION" "LABELS;SOURCES;LINK" ${ARGN})
  _vehicle_sil_scaled_ctest_timeout(timeout ${ARG_TIMEOUT})
  add_executable(${target} ${ARG_SOURCES})
  target_link_libraries(${target} PRIVATE vehicle_sil_test_main ${ARG_LINK})
  vehicle_sil_target_defaults(${target})
  vehicle_sil_copy_runtime_dlls(${target})

  set(_caller_cmake "")
  if(VEHICLE_SIL_BATCH_UNIT_TESTS AND EXISTS "${CMAKE_CURRENT_LIST_FILE}")
    file(READ "${CMAKE_CURRENT_LIST_FILE}" _caller_cmake)
  endif()

  set(names "")
  foreach(source IN LISTS ARG_SOURCES)
    get_filename_component(path "${source}" ABSOLUTE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(STRINGS "${path}" cases REGEX "^[ \t]*TEST_CASE[ \t]*\\([ \t]*[A-Za-z0-9_]+[ \t]*\\)")
    set(_needs_process_isolation FALSE)
    if(VEHICLE_SIL_BATCH_UNIT_TESTS)
      # Keep files that may return SKIP_RETURN_CODE 77 (Skipped) or initialize the one-shot
      # process-global tracer singleton (trace::start) unbatched so each case gets its own process.
      file(STRINGS "${path}" _isolation_lines REGEX "Skipped[ \t]*\\(|trace::start[ \t]*\\(")
      if(_isolation_lines)
        set(_needs_process_isolation TRUE)
      endif()
    endif()
    set(_individual "")
    set(_batchable "")
    foreach(line IN LISTS cases)
      string(REGEX REPLACE "^[ \t]*TEST_CASE[ \t]*\\([ \t]*([A-Za-z0-9_]+)[ \t]*\\).*" "\\1" name "${line}")
      list(APPEND names ${name})
      set(_keep_single TRUE)
      if(VEHICLE_SIL_BATCH_UNIT_TESTS AND NOT _needs_process_isolation)
        set(_is_serial FALSE)
        if(ARG_RUN_SERIAL AND name MATCHES "${ARG_RUN_SERIAL}")
          set(_is_serial TRUE)
        endif()
        set(_has_custom_props FALSE)
        if(_caller_cmake)
          string(FIND "${_caller_cmake}" "${ARG_PREFIX}.${name}" _prop_pos)
          if(NOT _prop_pos EQUAL -1)
            string(REGEX MATCH "(^|[^A-Za-z0-9_.])${ARG_PREFIX}\\.${name}([^A-Za-z0-9_.]|$)" _prop_match "${_caller_cmake}")
            if(_prop_match)
              set(_has_custom_props TRUE)
            endif()
          endif()
        endif()
        if(NOT _is_serial AND NOT _has_custom_props)
          set(_keep_single FALSE)
        endif()
      endif()
      if(_keep_single)
        list(APPEND _individual ${name})
      else()
        list(APPEND _batchable ${name})
      endif()
    endforeach()
    list(LENGTH _batchable _batch_len)
    if(_batch_len EQUAL 1)
      list(APPEND _individual ${_batchable})
      set(_batchable "")
      set(_batch_len 0)
    endif()
    foreach(name IN LISTS _individual)
      set(test "${ARG_PREFIX}.${name}")
      add_test(NAME ${test} COMMAND ${target} ${name})
      # 77: the test cannot run on this machine (vehicle_sil::unit::Skipped).
      set_tests_properties(${test} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${timeout} SKIP_RETURN_CODE 77)
      _vehicle_sil_tell_ctest_timeout_factor(${test})
      if(ARG_RUN_SERIAL AND name MATCHES "${ARG_RUN_SERIAL}")
        set_tests_properties(${test} PROPERTIES RUN_SERIAL TRUE)
      endif()
    endforeach()
    if(_batch_len GREATER 1)
      get_filename_component(_stem "${source}" NAME_WE)
      set(_batch_name "${ARG_PREFIX}.batch.${_stem}")
      get_property(_seen_batches GLOBAL PROPERTY VEHICLE_SIL_BATCH_TEST_NAMES)
      if(TEST "${_batch_name}" OR "${_batch_name}" IN_LIST _seen_batches)
        set(_batch_name "${ARG_PREFIX}.batch.${target}.${_stem}")
      endif()
      set_property(GLOBAL APPEND PROPERTY VEHICLE_SIL_BATCH_TEST_NAMES "${_batch_name}")
      list(JOIN _batchable "," _cases_csv)
      add_test(NAME ${_batch_name} COMMAND ${target} --cases ${_cases_csv})
      set_tests_properties(${_batch_name} PROPERTIES LABELS "${ARG_LABELS}" TIMEOUT ${timeout} SKIP_RETURN_CODE 77)
      _vehicle_sil_tell_ctest_timeout_factor(${_batch_name})
    endif()
  endforeach()

  # Guards the scan above: every test case in the binary must have been registered.
  # Several targets may share a prefix: the first one's check is <PREFIX>.registration, the
  # others' <PREFIX>.registration.<target>.
  list(JOIN names "|" expected)
  set(registration ${ARG_PREFIX}.registration)
  if(ARG_REGISTRATION)
    set(registration ${ARG_REGISTRATION})
  elseif(TEST ${registration})
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
# A launcher empties its work directory when it starts, so one build directory takes one ctest at a
# time: a second run of the same scenario there would delete the files of the first.
#
#   vehicle_sil_add_scenario_tests(<launcher>
#     PREFIX <prefix> LABELS <label>... TIMEOUT <seconds> WORK_DIR <dir>
#     SCENARIOS <name>... ARGS <arg>... [RUN_SERIAL] [MACHINE_LOCK <name>] [PROCESSORS <n>])
#
# MACHINE_LOCK: for scenarios that need something of which a machine has one, such as a fixed port
# that a document shows. Each test first takes the lock file /tmp/vehicle-sil-test-<name>.lock
# (flock, through cmake/vehicle_sil_machine_lock.sh), so that the builds of several working trees
# on one machine run such tests one at a time; RUN_SERIAL or RESOURCE_LOCK would hold only within
# one ctest. While it waits, the test says so, and the wait counts toward its TIMEOUT. Not on
# Windows; without flock, or when the file cannot be opened (another user's), the tests run
# without the lock (the script says so).
function(vehicle_sil_add_scenario_tests launcher)
  cmake_parse_arguments(ARG "RUN_SERIAL" "PREFIX;TIMEOUT;WORK_DIR;MACHINE_LOCK;PROCESSORS" "LABELS;SCENARIOS;ARGS" ${ARGN})
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
    if(ARG_PROCESSORS)
      set_tests_properties(${test} PROPERTIES PROCESSORS ${ARG_PROCESSORS})
    endif()
    _vehicle_sil_tell_ctest_timeout_factor(${test})
  endforeach()
endfunction()
