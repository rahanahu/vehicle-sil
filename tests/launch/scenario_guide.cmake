# The guide docs/howto/scenario-tests.md against the launcher and the examples it follows. DOC is the
# guide, ECU_MAIN the build directory of examples/ecu_main (simulation.yaml, scenarios/, engine/ and
# dashboard/ with the ECUs), ECU_MAIN_SOURCE examples/ecu_main, EXAMPLE examples/scenario_tests (the
# files the guide has its reader write), LAUNCH the launcher, WORK a directory this script makes anew.
# The script does what the reader does, in WORK, in the guide's order. Each block of the guide is the
# one after a line "<!-- ... -->" there:
#   the files      a scenario without its comment lines is the guide's block (the reader's file has no
#                  comments, and the lines in the outputs are counted in it); simulation.variants.yaml
#                  without comments is examples/ecu_main/simulation.yaml without comments and the
#                  lines the guide appends
#   the outputs    whole (test, the first run), from the line "run:" on, or as an excerpt that the
#                  output contains; the exit codes are the ones the guide names
# Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var LAUNCH DOC ECU_MAIN ECU_MAIN_SOURCE EXAMPLE WORK)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "scenario_guide.cmake: ${var} is required")
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

# The text of the file PATH without the lines that start with "#".
function(without_comments out path)
  read_lf(text "${path}")
  string(REGEX REPLACE "(^|\n)#[^\n]*" "" text "${text}")
  string(REGEX REPLACE "^\n" "" text "${text}")
  set(${out} "${text}" PARENT_SCOPE)
endfunction()

function(expect_same what actual marker)
  block_after(expected "${marker}")
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "${what} is\n${actual}\nand ${DOC} shows, after <!-- ${marker} -->,\n${expected}\n"
                        "Make them the same (the guide follows the examples and the launcher)")
  endif()
endfunction()

# The launcher in WORK with the arguments ARGN: its exit code is EXPECTED_CODE; its standard output is
# OUTPUT in the caller.
function(launch expected_code)
  execute_process(COMMAND "${LAUNCH}" ${ARGN} WORKING_DIRECTORY "${WORK}"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r\n" "\n" out "${out}")
  string(REPLACE ";" " " command "${ARGN}")
  set(output "${out}" PARENT_SCOPE)
  set(command "vehicle_sil_launch ${command}" PARENT_SCOPE)
  if(NOT code STREQUAL expected_code)
    message(FATAL_ERROR "vehicle_sil_launch ${command} (in ${WORK}) ended with ${code}; the guide says "
                        "${expected_code}\n--- standard output:\n${out}--- standard error:\n${err}")
  endif()
endfunction()

# The output of the last launch is the guide's block.
function(output_is marker)
  expect_same("the output of ${command}" "${output}" "${marker}")
endfunction()

# The output of the last launch from its line "run:" on is the guide's block.
function(output_from_run_is marker)
  string(FIND "${output}" "run: " at)
  if(at EQUAL -1)
    set(at 0)
  endif()
  string(SUBSTRING "${output}" ${at} -1 tail)
  expect_same("the output of ${command}, from the line 'run:'," "${tail}" "${marker}")
endfunction()

# The output of the last launch has the guide's block in it, as whole lines.
function(output_has marker)
  block_after(expected "${marker}")
  string(FIND "\n${output}" "\n${expected}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${command} wrote\n${output}\nwithout what ${DOC} shows after <!-- ${marker} -->:\n"
                        "${expected}\nMake them the same (the guide follows the launcher)")
  endif()
endfunction()

# The reader's file my_tests/NAME: the example's file without its comments, which is the guide's block.
function(write_scenario name)
  without_comments(text "${EXAMPLE}/my_tests/${name}")
  expect_same("examples/scenario_tests/my_tests/${name} (without comments)" "${text}"
              "examples/scenario_tests/my_tests/${name}")
  file(WRITE "${WORK}/my_tests/${name}" "${text}")
endfunction()

# WORK: the directory the reader works in (the build directory of examples/ecu_main), without what
# CMake keeps there.
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}/my_tests")
file(COPY "${ECU_MAIN}/engine" "${ECU_MAIN}/dashboard" "${ECU_MAIN}/scenarios" "${ECU_MAIN}/simulation.yaml"
     DESTINATION "${WORK}" PATTERN "CMakeFiles" EXCLUDE PATTERN "*.cmake" EXCLUDE)

# 1: the scenario that comes with the example.
without_comments(text "${ECU_MAIN_SOURCE}/scenarios/throttle_step.yaml")
expect_same("examples/ecu_main/scenarios/throttle_step.yaml (without comments)" "${text}"
            "examples/ecu_main/scenarios/throttle_step.yaml")
launch(0 run simulation.yaml --scenario scenarios/throttle_step.yaml)
output_is("vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml")

# 2: the variables.
launch(0 check simulation.yaml --json)
output_has("scenario guide: check simulation.yaml --json, one variable")

# 3: a scenario of the reader's, and a mistake in a name.
write_scenario(throttle_up.yaml)
launch(0 check simulation.yaml --scenario my_tests/throttle_up.yaml)
output_has("scenario guide: check simulation.yaml --scenario my_tests/throttle_up.yaml, the end")
launch(0 run simulation.yaml --scenario my_tests/throttle_up.yaml)
output_from_run_is("scenario guide: run simulation.yaml --scenario my_tests/throttle_up.yaml, from the line run:")
read_lf(text "${WORK}/my_tests/throttle_up.yaml")
string(REPLACE "engine.throttle" "engine.throttel" text "${text}")
file(WRITE "${WORK}/mistake.yaml" "${text}")
launch(1 check simulation.yaml --scenario mistake.yaml)
output_has("scenario guide: check simulation.yaml --scenario mistake.yaml, the end")

# 4: a scenario that fails, and the same with the time the engine needs.
without_comments(fixed "${EXAMPLE}/my_tests/response_time.yaml")
string(REPLACE "within_ms: 400" "within_ms: 200" failing "${fixed}")
if(failing STREQUAL fixed)
  message(FATAL_ERROR "examples/scenario_tests/my_tests/response_time.yaml has no 'within_ms: 400', which "
                      "${DOC} has its reader write in place of 200")
endif()
expect_same("examples/scenario_tests/my_tests/response_time.yaml (without comments, with within_ms: 200)"
            "${failing}" "scenario guide: my_tests/response_time.yaml with within_ms: 200")
file(WRITE "${WORK}/my_tests/response_time.yaml" "${failing}")
launch(1 run simulation.yaml --scenario my_tests/response_time.yaml)
output_from_run_is("scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml with within_ms: 200, from the line run:")
file(WRITE "${WORK}/my_tests/response_time.yaml" "${fixed}")
launch(0 run simulation.yaml --scenario my_tests/response_time.yaml)
output_has("scenario guide: run simulation.yaml --scenario my_tests/response_time.yaml, the result")

# 5: a ramp and a wave with an expect beside them, and a value in hexadecimal.
write_scenario(throttle_motion.yaml)
launch(0 run simulation.yaml --scenario my_tests/throttle_motion.yaml)
output_has("scenario guide: run simulation.yaml --scenario my_tests/throttle_motion.yaml, the result")

# 6: variants, parameters and only.
without_comments(text "${EXAMPLE}/simulation.variants.yaml")
without_comments(common "${ECU_MAIN_SOURCE}/simulation.yaml")
block_after(added "examples/scenario_tests/simulation.variants.yaml: the lines added")
if(NOT text STREQUAL "${common}${added}")
  message(FATAL_ERROR "examples/scenario_tests/simulation.variants.yaml (without comments) is\n${text}\nand not "
                      "examples/ecu_main/simulation.yaml (without comments) with the lines ${DOC} appends:\n${added}")
endif()
read_lf(text "${DOC}")
string(FIND "${text}" "<<'EOF'\n${added}EOF\n" at)
if(at EQUAL -1)
  message(FATAL_ERROR "${DOC}: the lines of the command that appends to simulation.variants.yaml (between "
                      "<<'EOF' and EOF) are not the block after <!-- examples/scenario_tests/"
                      "simulation.variants.yaml: the lines added -->")
endif()
read_lf(text "${WORK}/simulation.yaml")
file(WRITE "${WORK}/simulation.variants.yaml" "${text}${added}")
write_scenario(dashboard_speed.yaml)
launch(0 run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml)
launch(0 run simulation.variants.yaml --variant full --scenario my_tests/dashboard_speed.yaml
       --param throttle=30 --param speed=1400)
output_has("scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the parameters")
output_has("scenario guide: run --variant full --scenario my_tests/dashboard_speed.yaml --param throttle=30 --param speed=1400, the result")
launch(1 run simulation.variants.yaml --variant engine_only --scenario my_tests/dashboard_speed.yaml)
output_has("scenario guide: run --variant engine_only --scenario my_tests/dashboard_speed.yaml, the error")
launch(4 run simulation.variants.yaml --scenario my_tests/dashboard_speed.yaml)
launch(4 run simulation.yaml --scenario my_tests/throttle_up.yaml --param x=1)

# 7: every scenario of the directory with every variant.
launch(0 test simulation.variants.yaml --scenarios my_tests/)
output_is("scenario guide: test simulation.variants.yaml --scenarios my_tests/")
launch(1 test simulation.variants.yaml --scenarios my_tests/ --param speed=3000)
output_is("scenario guide: test simulation.variants.yaml --scenarios my_tests/ --param speed=3000")

# 8: the exit codes of the guide's table that the steps above have not shown, and the JSON: every key
# of the guide's excerpt has the value the launcher writes (of runs, the first one).
launch(3 run no-such-file.yaml)
launch(4 run simulation.yaml --scenario my_tests/throttle_up.yaml --start-paused)
launch(1 test simulation.variants.yaml --scenarios my_tests/ --param speed=3000 --json)
block_after(expected "scenario guide: test --json, an excerpt")
function(same_member)
  string(JSON value GET "${expected}" ${ARGN})
  string(JSON actual ERROR_VARIABLE error GET "${output}" ${ARGN})
  if(error OR NOT value STREQUAL actual)
    string(REPLACE ";" "/" where "${ARGN}")
    message(FATAL_ERROR "test --json: ${DOC} shows /${where} as '${value}'; the launcher writes '${actual}' ${error}")
  endif()
endfunction()
string(JSON count LENGTH "${expected}")
math(EXPR last "${count} - 1")
foreach(i RANGE ${last})
  string(JSON key MEMBER "${expected}" ${i})
  if(key STREQUAL "runs")
    string(JSON run_count LENGTH "${expected}" runs 0)
    math(EXPR run_last "${run_count} - 1")
    foreach(j RANGE ${run_last})
      string(JSON run_key MEMBER "${expected}" runs 0 ${j})
      same_member(runs 0 ${run_key})
    endforeach()
  else()
    same_member(${key})
  endif()
endforeach()

# Taking an ECU off its buses.
without_comments(text "${ECU_MAIN_SOURCE}/scenarios/engine_disconnect.yaml")
expect_same("examples/ecu_main/scenarios/engine_disconnect.yaml (without comments)" "${text}"
            "examples/ecu_main/scenarios/engine_disconnect.yaml")
launch(0 run simulation.yaml --scenario scenarios/engine_disconnect.yaml)
output_has("scenario guide: run simulation.yaml --scenario scenarios/engine_disconnect.yaml, the result")
message(STATUS "the scenario guide: the files, the outputs and the exit codes are the guide's")
