# The profiling trace of a whole run (docs/internals/trace.md): vehicle_sil_launch runs the two ECUs of
# examples/ecu_main with a scenario that writes, with VEHICLE_SIL_TRACE set. The launcher and each ECU must
# each have written a whole file with their own records at exit; with PYTHON, scripts/trace-report.py must
# summarise the directory. (The launcher's flush before std::_Exit, for a run whose thread did not close,
# is not reached here.)
# LAUNCH the launcher, ECU_MAIN the build directory of examples/ecu_main, WORK a directory this script makes
# anew, SCRIPT scripts/trace-report.py, PYTHON a Python 3.11+ or empty. Invoked by CTest with cmake -P.
cmake_minimum_required(VERSION 3.21)
foreach(var LAUNCH ECU_MAIN WORK SCRIPT)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "trace_files.cmake: ${var} is required")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
set(trace "${WORK}/trace")
execute_process(
  COMMAND ${CMAKE_COMMAND} -E env "VEHICLE_SIL_TRACE=${trace}"
          "${LAUNCH}" run "${ECU_MAIN}/simulation.yaml" --scenario "${ECU_MAIN}/scenarios/throttle_step.yaml"
          --speed max --log-dir "${WORK}/logs"
  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "vehicle_sil_launch run exited with ${code}:\n${out}\n${err}")
endif()

file(GLOB files "${trace}/trace-*.json")
list(LENGTH files n)
if(NOT n EQUAL 3)
  message(FATAL_ERROR "expected 3 trace files (the launcher's SimulationController, Engine, Dashboard) in ${trace}, found ${n}: ${files}\n"
                      "A process that ends without atexit must call trace::flush() first")
endif()

# <name>'s file holds every record named in the remaining arguments.
function(expect_records name)
  file(GLOB file "${trace}/trace-${name}-*.json")
  if(NOT file)
    message(FATAL_ERROR "no trace file of ${name} in ${trace}")
  endif()
  file(READ "${file}" text)
  string(LENGTH "${text}" length)
  math(EXPR tail_at "${length} - 4")
  string(SUBSTRING "${text}" ${tail_at} 4 tail)
  if(NOT tail STREQUAL "\n]}\n")
    message(FATAL_ERROR "${file} does not end the JSON: the file was cut off")
  endif()
  foreach(record IN LISTS ARGN)
    string(FIND "${text}" "\"name\":\"${record}\"" at)
    if(at EQUAL -1)
      message(FATAL_ERROR "${file} has no record ${record} (docs/internals/trace.md, \"Records\")")
    endif()
  endforeach()
endfunction()

expect_records(SimulationController step step.release gate.release scenario.tick scenario.write hub.write hub.send
               hub.receive_step hub.receive_answer)
expect_records(Engine step step.handler agent.step_begin agent.step_end agent.publish agent.receive_command agent.apply)
expect_records(Dashboard step step.handler agent.step_end agent.publish)

if(PYTHON)
  execute_process(COMMAND "${PYTHON}" -B "${SCRIPT}" "${trace}" --skip-ms 0 --json "${WORK}/report.json"
                  RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  if(NOT code EQUAL 0 OR NOT out MATCHES "^trace-report: 3 processes \\(launcher: SimulationController; ECUs: ")
    message(FATAL_ERROR "scripts/trace-report.py exited with ${code}:\n${out}\n${err}")
  endif()
  if(NOT out MATCHES "launcher hold, steps with writes" OR NOT out MATCHES "write round trip")
    message(FATAL_ERROR "scripts/trace-report.py found no held step with writes, or no write round trip:\n${out}")
  endif()
endif()
