# Checks and runs the template's simulation (DIR: simulation.yaml, scenarios/ and bin/ with the ECUs)
# with the launcher LAUNCH:
#   check simulation.yaml        exit code 0; with DOC (docs/howto/first-ecu.md), the output is the block after
#                                "<!-- vehicle_sil_launch check simulation.yaml -->" there
#   run simulation.yaml          the same with "<!-- vehicle_sil_launch run simulation.yaml -->"
#   run simulation.yaml --scenario scenarios/throttle_step.yaml   exit code 0; with SCENARIO_DOC
#                                (docs/scenario.md), the output is the block after the line with this command
# With DOC, simulation.yaml is also the block after "<!-- template/simulation.yaml -->" there.
# Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21) # cmake -P sets no policies without it (CMake 3 then rejects IN_LIST)
foreach(var LAUNCH DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "check_and_run.cmake: ${var} is required")
  endif()
endforeach()

# The fenced block after the line MARKER in the file DOC_FILE, without the fences ("\n" line ends).
function(block_after out marker)
  set(DOC "${DOC_FILE}")
  file(READ "${DOC}" text)
  string(REPLACE "\r\n" "\n" text "${text}")
  string(FIND "${text}" "${marker}\n" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${DOC} has no line ${marker}: put it back before the block that shows this output")
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

function(launch expected_code)
  execute_process(COMMAND "${LAUNCH}" ${ARGN} WORKING_DIRECTORY "${DIR}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r\n" "\n" out "${out}")
  set(output "${out}" PARENT_SCOPE)
  if(NOT code STREQUAL expected_code)
    message(FATAL_ERROR "vehicle_sil_launch ${ARGN} (in ${DIR}) ended with ${code}, not ${expected_code}\n"
                        "--- standard output:\n${out}--- standard error:\n${err}")
  endif()
endfunction()

function(same_as_doc doc command)
  if(NOT doc)
    return()
  endif()
  set(DOC_FILE "${doc}")
  block_after(expected "<!-- vehicle_sil_launch ${command} -->")
  if(NOT output STREQUAL expected)
    message(FATAL_ERROR "vehicle_sil_launch ${command} on the template wrote\n${output}\nand ${doc} shows\n"
                        "${expected}\nMake them the same: the docs follow the template")
  endif()
endfunction()

if(DOC)
  set(DOC_FILE "${DOC}")
  block_after(expected "<!-- template/simulation.yaml -->")
  file(READ "${DIR}/simulation.yaml" actual)
  string(REPLACE "\r\n" "\n" actual "${actual}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "template/simulation.yaml is\n${actual}\nand ${DOC} shows\n${expected}\nMake them the same")
  endif()
endif()
launch(0 check simulation.yaml)
same_as_doc("${DOC}" "check simulation.yaml")
launch(0 run simulation.yaml)
same_as_doc("${DOC}" "run simulation.yaml")
launch(0 run simulation.yaml --scenario scenarios/throttle_step.yaml)
same_as_doc("${SCENARIO_DOC}" "run simulation.yaml --scenario scenarios/throttle_step.yaml")
message(STATUS "the template's simulation: check, run and run --scenario passed")
