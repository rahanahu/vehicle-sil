# Copies template/ of SOURCE_DIR (the vehicle-sil working tree) to BINARY_DIR/project, configures and
# builds it with its own preset (linux or windows) and vehicle-sil from SOURCE_DIR instead of the
# network (FETCHCONTENT_SOURCE_DIR_VEHICLE_SIL), then checks and runs its simulation with LAUNCH
# (tests/template/check_and_run.cmake). Invoked by CTest with cmake -P.
foreach(var SOURCE_DIR BINARY_DIR LAUNCH)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_template.cmake: ${var} is required")
  endif()
endforeach()

set(project "${BINARY_DIR}/project")
file(REMOVE_RECURSE "${BINARY_DIR}")
# What a user copies: everything but what building and fetching in place may have left there.
file(GLOB_RECURSE files RELATIVE "${SOURCE_DIR}/template" LIST_DIRECTORIES false "${SOURCE_DIR}/template/*")
foreach(file IN LISTS files)
  if(NOT file MATCHES "^(build|bin|tools)/|__pycache__")
    get_filename_component(dir "${project}/${file}" DIRECTORY)
    file(COPY "${SOURCE_DIR}/template/${file}" DESTINATION "${dir}")
  endif()
endforeach()

if(WIN32)
  set(preset windows)
else()
  set(preset linux)
endif()
set(configure_args --preset ${preset} "-DFETCHCONTENT_SOURCE_DIR_VEHICLE_SIL=${SOURCE_DIR}")
if(C_COMPILER)
  list(APPEND configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(CXX_COMPILER)
  list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} WORKING_DIRECTORY "${project}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build --preset ${preset} WORKING_DIRECTORY "${project}"
                COMMAND_ERROR_IS_FATAL ANY)

set(DIR "${project}")
set(DOC "${SOURCE_DIR}/docs/howto/first-ecu.md")
set(SCENARIO_DOC "${SOURCE_DIR}/docs/scenario.md")
include("${SOURCE_DIR}/tests/template/check_and_run.cmake")
