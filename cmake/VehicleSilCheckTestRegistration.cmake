# Script mode (cmake -P): fails if the test cases compiled into EXE differ from the names
# registered with CTest at configure time (EXPECTED, separated by '|'). Catches a TEST_CASE that
# the configure-time scan did not recognise. EMULATOR (optional, a ';'-list) runs EXE, as
# CMAKE_CROSSCOMPILING_EMULATOR does for the other tests in a cross build.
execute_process(COMMAND ${EMULATOR} "${EXE}" --list OUTPUT_VARIABLE listed RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "${EXE} --list failed with ${result}")
endif()
string(REGEX REPLACE "\r?\n$" "" listed "${listed}")
string(REGEX REPLACE "\r?\n" ";" listed "${listed}")
string(REPLACE "|" ";" expected "${EXPECTED}")
list(SORT listed)
list(SORT expected)
if(NOT listed STREQUAL expected)
  message(FATAL_ERROR "registered tests differ from the test cases in ${EXE}\n  registered: ${expected}\n  compiled:   ${listed}")
endif()
message(STATUS "all ${EXE} test cases are registered")
