# SPDX-License-Identifier: MIT
# Unit tests for cmake/VehicleSilTestHelpers.cmake timeout-scale parsing and CTest TIMEOUT scaling.
# Must remain compatible with CMake 3.21.

if(NOT DEFINED HELPER)
  get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
  set(HELPER "${_repo}/cmake/VehicleSilTestHelpers.cmake")
endif()

if(MODE STREQUAL "child_default")
  set(ENV{VEHICLE_SIL_TEST_TIMEOUT_SCALE} "")
  include("${HELPER}")
  get_property(_factor GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR)
  _vehicle_sil_scaled_ctest_timeout(_scaled 300)
  message(STATUS "RESULT:factor=${_factor}:scaled=${_scaled}")
  return()
elseif(MODE STREQUAL "child_env")
  set(ENV{VEHICLE_SIL_TEST_TIMEOUT_SCALE} "${TEST_ENV_SCALE}")
  include("${HELPER}")
  get_property(_factor GLOBAL PROPERTY VEHICLE_SIL_TEST_TIMEOUT_FACTOR)
  _vehicle_sil_scaled_ctest_timeout(_scaled 300)
  message(STATUS "RESULT:factor=${_factor}:scaled=${_scaled}")
  return()
endif()

set(ENV{VEHICLE_SIL_TEST_TIMEOUT_SCALE} "")
include("${HELPER}")

function(_expect_factor text expected_factor expected_ok)
  _vehicle_sil_whole_timeout_factor("${text}" actual_factor actual_ok)
  if(NOT actual_ok STREQUAL expected_ok OR NOT actual_factor STREQUAL expected_factor)
    message(FATAL_ERROR "_vehicle_sil_whole_timeout_factor('${text}') -> (${actual_factor}, ${actual_ok}), expected (${expected_factor}, ${expected_ok})")
  endif()
endfunction()

# Valid inputs (rounded up to whole integer >= 1, clamped at 1000000000).
_expect_factor("3" "3" "TRUE")
_expect_factor("0.5" "1" "TRUE")
_expect_factor(".5" "1" "TRUE")
_expect_factor("5." "5" "TRUE")
_expect_factor("007" "7" "TRUE")
_expect_factor("0.001" "1" "TRUE")
_expect_factor("2.5" "3" "TRUE")
_expect_factor("2.0" "2" "TRUE")
_expect_factor("0.0001" "1" "TRUE")

string(REPEAT "0" 308 _zeros_308)
string(REPEAT "0" 323 _zeros_323)
string(REPEAT "0" 324 _zeros_324)
string(REPEAT "0" 400 _zeros_400)
string(REPEAT "9" 18 _nines_18)
string(REPEAT "9" 19 _nines_19)
string(REPEAT "0" 17 _zeros_17)
string(REPEAT "0" 309 _zeros_309)
string(REPEAT "9" 400 _nines_400)

_expect_factor("999999999" "999999999" "TRUE")
_expect_factor("999999999.5" "1000000000" "TRUE")
_expect_factor("1000000000" "1000000000" "TRUE")
_expect_factor("1000000000.5" "1000000000" "TRUE")
_expect_factor("1${_zeros_17}" "1000000000" "TRUE")
_expect_factor("${_nines_18}" "1000000000" "TRUE")
_expect_factor("${_nines_19}.5" "1000000000" "TRUE")
_expect_factor("1${_zeros_308}" "1000000000" "TRUE")
_expect_factor("0.${_zeros_308}1" "1" "TRUE")
_expect_factor("0.${_zeros_323}5" "1" "TRUE")
_expect_factor("0.${_zeros_323}2470328230" "1" "TRUE")

# Invalid inputs.
foreach(_bad IN ITEMS "" "0" "0.0" "000" "000.0" "." "-1" "+3" "abc" "3x" " " " 3" "3 " "1e3" "1e-3" "0x10" "1_000" "1.2.3" "inf" "nan" "Infinity" "NaN"
                      "${_nines_400}" "1${_zeros_309}" "2${_zeros_308}" "0.${_zeros_400}1" "0.${_zeros_324}1" ".${_zeros_400}1"
                      "0.${_zeros_323}1" "0.${_zeros_323}2470328229")
  _expect_factor("${_bad}" "1" "FALSE")
endforeach()
_expect_factor("3\n" "1" "FALSE")
_expect_factor("٢" "1" "FALSE")
_expect_factor("²" "1" "FALSE")

# Default fallback without env override.
execute_process(
  COMMAND ${CMAKE_COMMAND} -DHELPER=${HELPER} -DMODE=child_default -DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4
          -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "RESULT:factor=4:scaled=1200")
  message(FATAL_ERROR "child_default failed: rc=${_rc}\nout=${_out}\nerr=${_err}")
endif()

# Valid env override supersedes default.
execute_process(
  COMMAND ${CMAKE_COMMAND} -DHELPER=${HELPER} -DMODE=child_env -DTEST_ENV_SCALE=2.5 -DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4
          -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0 OR NOT _out MATCHES "RESULT:factor=3:scaled=900")
  message(FATAL_ERROR "child_env failed: rc=${_rc}\nout=${_out}\nerr=${_err}")
endif()

# Large valid env factors clamp to 1000000000 without math(EXPR) overflow or 64-bit wraparound.
foreach(_large IN ITEMS "${_nines_19}.5" "1${_zeros_17}" "${_nines_18}")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DHELPER=${HELPER} -DMODE=child_env -DTEST_ENV_SCALE=${_large} -DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4
            -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0 OR NOT _out MATCHES "RESULT:factor=1000000000:scaled=300000000000")
    message(FATAL_ERROR "child_env large (${_large}) failed: rc=${_rc}\nout=${_out}\nerr=${_err}")
  endif()
endforeach()

# Invalid env (including overflow and underflow) warns and falls back to default.
foreach(_bad_env IN ITEMS "bad" "${_nines_400}" "0.${_zeros_400}1")
  execute_process(
    COMMAND ${CMAKE_COMMAND} -DHELPER=${HELPER} -DMODE=child_env -DTEST_ENV_SCALE=${_bad_env} -DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=4
            -P "${CMAKE_CURRENT_LIST_FILE}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0 OR NOT _out MATCHES "RESULT:factor=4:scaled=1200" OR NOT _err MATCHES "VEHICLE_SIL_TEST_TIMEOUT_SCALE=\"${_bad_env}\"")
    message(FATAL_ERROR "child_env invalid (${_bad_env}) failed: rc=${_rc}\nout=${_out}\nerr=${_err}")
  endif()
endforeach()

# Invalid build default is a fatal error.
execute_process(
  COMMAND ${CMAKE_COMMAND} -DHELPER=${HELPER} -DMODE=child_default -DVEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=bad
          -P "${CMAKE_CURRENT_LIST_FILE}"
  RESULT_VARIABLE _rc
  OUTPUT_VARIABLE _out
  ERROR_VARIABLE _err)
if(_rc EQUAL 0 OR NOT _err MATCHES "VEHICLE_SIL_TEST_TIMEOUT_SCALE_DEFAULT=\"bad\"")
  message(FATAL_ERROR "child_default bad should fail: rc=${_rc}\nout=${_out}\nerr=${_err}")
endif()
