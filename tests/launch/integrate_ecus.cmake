# The example of the guide docs/howto/integrate-ecus.md (examples/integrate_ecus) against the guide.
# DOC is the guide, EXAMPLE the example's source directory, DIR its build directory (the three
# configurations and the scenario next to dashboard_v1/, with ../ecu_main beside it), DASHBOARD_V1
# the built vehicle_sil_dashboard_v1_ecu, LAUNCH the launcher. Each block of the guide is the one
# after a line "<!-- ... -->" there:
#   the files      simulation.yaml and scenarios/engine_speed.yaml are the guide's blocks;
#                  simulation.two-buses.yaml and simulation.fixed.yaml have the lines the guide shows;
#                  simulation.network-names.yaml is simulation.yaml with the guide's Dashboard;
#                  simulation.second-engine.yaml has the guide's Engine2 (with j1939_nodes)
#   --describe     every key of the guide's excerpt has the value vehicle_sil_dashboard_v1_ecu writes
#   check          of the five configurations (exit codes 1, 0, 0, 0, 0), with --disable Dashboard, and
#                  --json (every key of the guide's excerpt): the outputs are the guide's
#   run            of simulation.two-buses.yaml and simulation.fixed.yaml, alone (exit code 0 both,
#                  the dashboard's lines differ) and with the scenario (exit codes 1 and 0)
# Nothing is written to DIR. Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var LAUNCH DOC EXAMPLE DIR DASHBOARD_V1)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "integrate_ecus.cmake: ${var} is required")
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
                        "(the guide follows examples/integrate_ecus and the launcher)")
  endif()
endfunction()

function(expect_within what text part)
  string(FIND "${text}" "${part}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${what} is\n${text}\nwithout what ${DOC} shows:\n${part}\nMake them the same "
                        "(the guide follows examples/integrate_ecus and the launcher)")
  endif()
endfunction()

# Runs ARGN in DIR: `output` is its standard output, `errors` its standard error.
function(run_in_dir expected_code)
  execute_process(COMMAND ${ARGN} WORKING_DIRECTORY "${DIR}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r\n" "\n" out "${out}")
  string(REPLACE "\r\n" "\n" err "${err}")
  set(output "${out}" PARENT_SCOPE)
  set(errors "${err}" PARENT_SCOPE)
  if(NOT code STREQUAL expected_code)
    string(REPLACE ";" " " command "${ARGN}")
    message(FATAL_ERROR "${command} (in ${DIR}) ended with ${code}, not ${expected_code}\n"
                        "--- standard output:\n${out}--- standard error:\n${err}")
  endif()
endfunction()

# TEXT from the first FROM on (all of it without FROM).
function(tail_from out text from)
  string(FIND "${text}" "${from}" at)
  if(at EQUAL -1)
    set(at 0)
  endif()
  string(SUBSTRING "${text}" ${at} -1 text)
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The lines of TEXT that start with "[Dashboard] ", each with its line end.
function(dashboard_lines out text)
  string(REGEX MATCHALL "(^|\n)\\[Dashboard\\] [^\n]*" lines "${text}")
  set(result "")
  foreach(line IN LISTS lines)
    string(REGEX REPLACE "^\n" "" line "${line}")
    string(APPEND result "${line}\n")
  endforeach()
  set(${out} "${result}" PARENT_SCOPE)
endfunction()

# Every key of the JSON text EXPECTED (at the path ARGN) is in ACTUAL with the same value; an array
# has the same number of elements.
function(expect_json_within what expected actual)
  string(REPLACE ";" "/" where "${ARGN}")
  string(JSON type TYPE "${expected}" ${ARGN})
  string(JSON actual_type ERROR_VARIABLE error TYPE "${actual}" ${ARGN})
  if(error OR NOT type STREQUAL actual_type)
    message(FATAL_ERROR "${what}: ${DOC} shows /${where} as ${type}; the program writes ${actual_type} ${error}")
  endif()
  if(type STREQUAL "OBJECT" OR type STREQUAL "ARRAY")
    string(JSON count LENGTH "${expected}" ${ARGN})
    if(type STREQUAL "ARRAY")
      string(JSON actual_count LENGTH "${actual}" ${ARGN})
      if(NOT count EQUAL actual_count)
        message(FATAL_ERROR "${what}: ${DOC} shows ${count} elements in /${where}; the program writes ${actual_count}")
      endif()
    endif()
    if(count GREATER 0)
      math(EXPR last "${count} - 1")
      foreach(i RANGE ${last})
        set(key ${i})
        if(type STREQUAL "OBJECT")
          string(JSON key MEMBER "${expected}" ${ARGN} ${i})
        endif()
        expect_json_within("${what}" "${expected}" "${actual}" ${ARGN} ${key})
      endforeach()
    endif()
  else()
    string(JSON value GET "${expected}" ${ARGN})
    string(JSON actual_value GET "${actual}" ${ARGN})
    if(NOT value STREQUAL actual_value)
      message(FATAL_ERROR "${what}: ${DOC} shows /${where} as '${value}'; the program writes '${actual_value}'")
    endif()
  endif()
endfunction()

# The files.
foreach(file simulation.yaml scenarios/engine_speed.yaml)
  read_lf(text "${EXAMPLE}/${file}")
  block_after(expected "examples/integrate_ecus/${file}")
  expect_same("examples/integrate_ecus/${file}" "${text}" "${expected}")
endforeach()
read_lf(text "${EXAMPLE}/simulation.two-buses.yaml")
block_after(expected "examples/integrate_ecus/simulation.two-buses.yaml: networks")
expect_within("examples/integrate_ecus/simulation.two-buses.yaml" "${text}" "\n${expected}participants:\n")
read_lf(text "${EXAMPLE}/simulation.fixed.yaml")
block_after(expected "examples/integrate_ecus/simulation.fixed.yaml: the command of the Dashboard")
expect_within("examples/integrate_ecus/simulation.fixed.yaml" "${text}" "  - name: Dashboard\n${expected}")
# simulation.yaml with the guide's lines of the Dashboard in place of its own (comments aside).
read_lf(text "${EXAMPLE}/simulation.network-names.yaml")
read_lf(first "${EXAMPLE}/simulation.yaml")
block_after(expected "examples/integrate_ecus/simulation.network-names.yaml: the participant Dashboard")
string(FIND "${first}" "  - name: Dashboard\n" at)
string(SUBSTRING "${first}" 0 ${at} first)
string(REGEX REPLACE "#[^\n]*\n" "" first "${first}${expected}")
string(REGEX REPLACE "#[^\n]*\n" "" text "${text}")
expect_same("examples/integrate_ecus/simulation.network-names.yaml without its comments" "${text}" "${first}")
read_lf(text "${EXAMPLE}/simulation.second-engine.yaml")
block_after(expected "examples/integrate_ecus/simulation.second-engine.yaml: the participant Engine2")
expect_within("examples/integrate_ecus/simulation.second-engine.yaml" "${text}" "\n${expected}  - name: Dashboard\n")
# and without it, simulation.fixed.yaml (comments aside).
string(REPLACE "${expected}" "" text "${text}")
string(REGEX REPLACE "#[^\n]*\n" "" text "${text}")
read_lf(fixed "${EXAMPLE}/simulation.fixed.yaml")
string(REGEX REPLACE "#[^\n]*\n" "" fixed "${fixed}")
expect_same("examples/integrate_ecus/simulation.second-engine.yaml without Engine2 and its comments" "${text}" "${fixed}")

# The ECU as first handed over.
run_in_dir(0 "${DASHBOARD_V1}" --describe)
block_after(expected "integrate ECUs: vehicle_sil_dashboard_v1_ecu --describe, an excerpt")
expect_json_within("vehicle_sil_dashboard_v1_ecu --describe" "${expected}" "${output}")

# check.
run_in_dir(1 "${LAUNCH}" check simulation.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.yaml")
expect_same("the output of vehicle_sil_launch check simulation.yaml" "${output}" "${expected}")
run_in_dir(1 "${LAUNCH}" check --json simulation.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check --json simulation.yaml, an excerpt")
expect_json_within("vehicle_sil_launch check --json simulation.yaml" "${expected}" "${output}")
run_in_dir(0 "${LAUNCH}" check simulation.two-buses.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.two-buses.yaml")
expect_same("the output of vehicle_sil_launch check simulation.two-buses.yaml" "${output}" "${expected}")
run_in_dir(1 "${LAUNCH}" check simulation.two-buses.yaml --strict)
read_lf(guide "${DOC}")
string(REGEX MATCH "\n(failed: [^\n]*\\(--strict\\))\n" last_line "${output}")
expect_within("the guide's text about --strict" "${guide}" "`${CMAKE_MATCH_1}`")
run_in_dir(0 "${LAUNCH}" check simulation.fixed.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml")
expect_same("the output of vehicle_sil_launch check simulation.fixed.yaml" "${output}" "${expected}")
run_in_dir(0 "${LAUNCH}" check simulation.network-names.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.network-names.yaml")
expect_same("the output of vehicle_sil_launch check simulation.network-names.yaml" "${output}" "${expected}")
run_in_dir(0 "${LAUNCH}" check simulation.second-engine.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.second-engine.yaml")
expect_same("the output of vehicle_sil_launch check simulation.second-engine.yaml" "${output}" "${expected}")
run_in_dir(0 "${LAUNCH}" check simulation.fixed.yaml --disable Dashboard)
block_after(expected "integrate ECUs: vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard")
expect_same("the output of vehicle_sil_launch check simulation.fixed.yaml --disable Dashboard" "${output}" "${expected}")

# run: both end with exit code 0; only the fixed one carries the engine speed to the dashboard.
foreach(config simulation.two-buses.yaml simulation.fixed.yaml)
  run_in_dir(0 "${LAUNCH}" run ${config} --stop-at-ms 1000)
  block_after(expected "integrate ECUs: vehicle_sil_launch run ${config}, from the line \"run:\"")
  tail_from(tail "${output}" "run: ")
  expect_same("the output of vehicle_sil_launch run ${config}, from the line 'run:'," "${tail}" "${expected}")
  block_after(expected "integrate ECUs: vehicle_sil_launch run ${config}, the lines of the Dashboard")
  dashboard_lines(lines "${errors}")
  expect_same("the lines of the Dashboard on the standard error of vehicle_sil_launch run ${config}" "${lines}"
              "${expected}")
endforeach()

# run --scenario.
set(result_line "scenario scenarios/engine_speed.yaml (the dashboard receives the engine speed): ")
run_in_dir(0 "${LAUNCH}" run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml, the result of the scenario")
expect_within("the output of vehicle_sil_launch run simulation.fixed.yaml --scenario scenarios/engine_speed.yaml"
              "${output}" "\n${expected}\nparticipant ")
run_in_dir(1 "${LAUNCH}" run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml)
block_after(expected "integrate ECUs: vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml, from the result of the scenario")
tail_from(tail "${output}" "${result_line}FAILED")
expect_same("the output of vehicle_sil_launch run simulation.two-buses.yaml --scenario scenarios/engine_speed.yaml, from the result of the scenario,"
            "${tail}" "${expected}")
message(STATUS "integrate ECUs: the files, --describe, check and run are the guide's")
