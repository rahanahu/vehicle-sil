# The example of the guide docs/howto/first-ecu.md (examples/first_ecu: the ECU "Cooler" added to the
# template's project) against the guide. DOC is the guide, EXAMPLE the example's directory, TEMPLATE
# the template's, BIN the directory with engine_ecu, dashboard_ecu and cooler_ecu, COOLER the built
# cooler_ecu, LAUNCH the launcher, WORK a directory this script makes anew. Each block of the guide is
# the one after a line "<!-- ... -->" there:
#   the files      cooler/main.cpp and read_cooler.py without their first line (the SPDX line) are the
#                  guide's blocks; CMakeLists.txt and simulation.yaml without comments are the lines the
#                  guide adds (for simulation.yaml: after the template's file without comments)
#   --describe     every key of the guide's excerpt has the value cooler_ecu writes
#   --local        exit code 0 and nothing on standard output
#   check, run     in WORK (the example's simulation.yaml next to a copy of BIN): the outputs are the guide's,
#                  also with step_ms written wrongly and with --disable Dashboard
# With PYTHON (a Python with grpcio; PYTHON_PATH and STUBS for the client), only this:
#   read_cooler.py in WORK, with the launcher on PATH: its output is the guide's
# Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var LAUNCH DOC EXAMPLE TEMPLATE BIN WORK)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "first_ecu.cmake: ${var} is required")
  endif()
endforeach()

function(read_lf out path)
  file(READ "${path}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The fenced block after the line "<!-- MARKER -->" in the guide, without the fences.
function(block_after out marker)
  read_lf(text "${DOC}")
  string(FIND "${text}" "<!-- ${marker} -->\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${DOC} has no line <!-- ${marker} -->: put it back before the block it marks")
  endif()
  string(SUBSTRING "${text}" ${at} -1 text)
  string(FIND "${text}" "```" open)
  string(SUBSTRING "${text}" ${open} -1 text)
  string(FIND "${text}" "\n" line_end)
  math(EXPR body_start "${line_end} + 1")
  string(SUBSTRING "${text}" ${body_start} -1 text)
  string(FIND "${text}" "```" close)
  string(SUBSTRING "${text}" 0 ${close} text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(expect_same what actual expected)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${what} is\n${actual}\nand ${DOC} shows\n${expected}\nMake them the same "
                        "(the guide follows examples/first_ecu and the launcher)")
  endif()
endfunction()

function(without_first_line out text)
  string(FIND "${text}" "\n" line_end)
  math(EXPR start "${line_end} + 1")
  string(SUBSTRING "${text}" ${start} -1 text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# Without the lines that start with "#" (what is left starts with a line end).
function(without_comments out text)
  string(REGEX REPLACE "(^|\n)#[^\n]*" "" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(run_in_work expected_code)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r\n" "\n" out "${out}")
  set(output "${out}" PARENT_SCOPE)
  if(NOT code STREQUAL expected_code)
    string(REPLACE ";" " " command "${ARGN}")
    message(FATAL_ERROR "${command} (in ${WORK}) ended with ${code}, not ${expected_code}\n"
                        "--- standard output:\n${out}--- standard error:\n${err}")
  endif()
endfunction()

# Every key of the JSON text EXPECTED (at the path ARGN) is in ACTUAL with the same value; an array
# has the same number of elements.
function(expect_json_within expected actual)
  string(REPLACE ";" "/" where "${ARGN}")
  string(JSON type TYPE "${expected}" ${ARGN})
  string(JSON actual_type ERROR_VARIABLE error TYPE "${actual}" ${ARGN})
  if(error OR NOT type STREQUAL actual_type)
    message(FATAL_ERROR "--describe: ${DOC} shows /${where} as ${type}; cooler_ecu writes ${actual_type} ${error}")
  endif()
  if(type STREQUAL "OBJECT" OR type STREQUAL "ARRAY")
    string(JSON count LENGTH "${expected}" ${ARGN})
    if(type STREQUAL "ARRAY")
      string(JSON actual_count LENGTH "${actual}" ${ARGN})
      if(NOT count EQUAL actual_count)
        message(FATAL_ERROR "--describe: ${DOC} shows ${count} elements in /${where}; cooler_ecu writes ${actual_count}")
      endif()
    endif()
    if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(i RANGE ${last})
        set(key ${i})
        if(type STREQUAL "OBJECT")
          string(JSON key MEMBER "${expected}" ${ARGN} ${i})
        endif()
        expect_json_within("${expected}" "${actual}" ${ARGN} ${key})
      endforeach()
    endif()
  else()
    string(JSON value GET "${expected}" ${ARGN})
    string(JSON actual_value GET "${actual}" ${ARGN})
    if(NOT value STREQUAL actual_value)
      message(FATAL_ERROR "--describe: ${DOC} shows /${where} as '${value}'; cooler_ecu writes '${actual_value}'")
    endif()
  endif()
endfunction()

# WORK: the project as the reader of the guide has it after adding the ECU.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
file(COPY "${BIN}" DESTINATION "${WORK}")
get_filename_component(bin_name "${BIN}" NAME)
if(NOT bin_name STREQUAL "bin")
  file(RENAME "${WORK}/${bin_name}" "${WORK}/bin")
endif()
file(COPY "${EXAMPLE}/simulation.yaml" DESTINATION "${WORK}")

if(PYTHON)
  get_filename_component(launch_dir "${LAUNCH}" DIRECTORY)
  if(CMAKE_HOST_WIN32)
    set(ENV{PATH} "${launch_dir};$ENV{PATH}")
  else()
    set(ENV{PATH} "${launch_dir}:$ENV{PATH}")
  endif()
  set(ENV{PYTHONPATH} "${PYTHON_PATH}")
  set(ENV{VEHICLE_SIL_CONTROL_STUBS} "${STUBS}")
  run_in_work(0 "${PYTHON}" "${EXAMPLE}/read_cooler.py")
  block_after(expected "examples/first_ecu/read_cooler.py: the output")
  expect_same("the output of examples/first_ecu/read_cooler.py" "${output}" "${expected}")
  message(STATUS "the first ECU's read_cooler.py: its output is the guide's")
  return()
endif()

# The files.
foreach(file cooler/main.cpp read_cooler.py)
  read_lf(text "${EXAMPLE}/${file}")
  without_first_line(text "${text}")
  block_after(expected "examples/first_ecu/${file}")
  expect_same("examples/first_ecu/${file} (without its first line)" "${text}" "${expected}")
endforeach()
read_lf(text "${EXAMPLE}/CMakeLists.txt")
without_comments(text "${text}")
block_after(expected "examples/first_ecu/CMakeLists.txt")
expect_same("examples/first_ecu/CMakeLists.txt (without comments)" "${text}" "\n${expected}")
read_lf(text "${EXAMPLE}/simulation.yaml")
without_comments(text "${text}")
read_lf(template "${TEMPLATE}/simulation.yaml")
without_comments(template "${template}")
block_after(expected "examples/first_ecu/simulation.yaml: the lines added")
expect_same("examples/first_ecu/simulation.yaml (without comments)" "${text}"
            "${template}${expected}")

# The ECU alone.
if(NOT COOLER)
  message(FATAL_ERROR "first_ecu.cmake: COOLER is required")
endif()
run_in_work(0 "${COOLER}" --describe)
block_after(expected "cooler_ecu --describe: an excerpt")
expect_json_within("${expected}" "${output}")
run_in_work(0 "${COOLER}" --local --run-for-ms 1000)
expect_same("the standard output of cooler_ecu --local --run-for-ms 1000" "${output}" "")

# The three ECUs.
run_in_work(0 "${LAUNCH}" check simulation.yaml)
block_after(expected "first ECU: vehicle_sil_launch check simulation.yaml")
expect_same("the output of vehicle_sil_launch check simulation.yaml with the Cooler" "${output}" "${expected}")

run_in_work(0 "${LAUNCH}" run simulation.yaml)
block_after(expected "first ECU: vehicle_sil_launch run simulation.yaml, after the tables of the check")
string(FIND "${output}" "run: " at)
if(at EQUAL -1)
  set(at 0)
endif()
string(SUBSTRING "${output}" ${at} -1 tail)
expect_same("the output of vehicle_sil_launch run simulation.yaml with the Cooler, from the line 'run:'," "${tail}"
            "${expected}")

run_in_work(0 "${LAUNCH}" check simulation.yaml --disable Dashboard)
block_after(expected "first ECU: vehicle_sil_launch check simulation.yaml --disable Dashboard, the warning")
string(FIND "${output}" "\n${expected}" at)
if(at EQUAL -1)
  message(FATAL_ERROR "vehicle_sil_launch check simulation.yaml --disable Dashboard wrote\n${output}\nwithout the "
                      "line ${DOC} shows:\n${expected}Make them the same")
endif()

read_lf(text "${WORK}/simulation.yaml")
string(REPLACE "step_ms: 100" "step_ms: 10" text "${text}")
file(WRITE "${WORK}/simulation.yaml" "${text}")
run_in_work(1 "${LAUNCH}" check simulation.yaml)
block_after(expected "first ECU: vehicle_sil_launch check simulation.yaml with step_ms: 10, the end")
string(FIND "${output}" "error: " at)
if(at EQUAL -1)
  set(at 0)
endif()
string(SUBSTRING "${output}" ${at} -1 tail)
expect_same("the output of vehicle_sil_launch check simulation.yaml with step_ms: 10, from the line 'error:'," "${tail}"
            "${expected}")
file(COPY "${EXAMPLE}/simulation.yaml" DESTINATION "${WORK}")
message(STATUS "the first ECU: the files, --describe, --local, check and run are the guide's")
