# The example of the guide docs/howto/multi-pc.md (examples/ecu_main/simulation.two-pcs.yaml) against
# the guide. DOC is the guide, EXAMPLE the file in the source tree, EXAMPLE_DIR the build directory of
# examples/ecu_main (with the copy of the file next to the two ECUs), LAUNCH the launcher. Each block
# of the guide is the one after a line "<!-- ... -->" there:
#   the file   simulation.two-pcs.yaml without its comments is the guide's block
#   check      in EXAMPLE_DIR: exit code 0 and the output the guide shows (the notes registry-on-network
#              and participant-manual)
# Nothing listens here (check starts no registry); tests/multi_host/two_hosts.py runs over two computers.
# Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var LAUNCH DOC EXAMPLE EXAMPLE_DIR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "docs.cmake: ${var} is required")
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
                        "(the guide follows examples/ecu_main/simulation.two-pcs.yaml and the launcher)")
  endif()
endfunction()

# The file: without the lines that start with "#", it is the guide's block.
read_lf(file_text "${EXAMPLE}")
string(REGEX REPLACE "(^|\n)#[^\n]*" "" file_text "${file_text}")
string(REGEX REPLACE "^\n" "" file_text "${file_text}")
block_after(shown "examples/ecu_main/simulation.two-pcs.yaml")
expect_same("${EXAMPLE} without its comments" "${file_text}" "${shown}")

# check: the output with the two notes.
execute_process(COMMAND "${LAUNCH}" check simulation.two-pcs.yaml WORKING_DIRECTORY "${EXAMPLE_DIR}"
                RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
string(REPLACE "\r\n" "\n" out "${out}")
if(NOT code STREQUAL "0")
  message(FATAL_ERROR "vehicle_sil_launch check simulation.two-pcs.yaml (in ${EXAMPLE_DIR}) ended with ${code}, not 0\n"
                      "--- standard output:\n${out}--- standard error:\n${err}")
endif()
block_after(shown "multi-pc guide: check simulation.two-pcs.yaml")
expect_same("the output of vehicle_sil_launch check simulation.two-pcs.yaml" "${out}" "${shown}")
