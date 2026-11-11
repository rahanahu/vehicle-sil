# Configures, builds and runs examples/fetchcontent_consumer in a fresh build
# tree, fetching vehicle-sil from REPOSITORY at TAG (HEAD is resolved to a commit
# hash first: with git rev-parse for a local repository, with git ls-remote for a
# URL). PYTHON, if given, is the consumer's Python3_EXECUTABLE (for the J1939 code generator).
# Invoked by CTest with cmake -P.
foreach(var SOURCE_DIR BINARY_DIR REPOSITORY TAG GENERATOR)
  if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
    message(FATAL_ERROR "run_consumer.cmake: ${var} is required")
  endif()
endforeach()

if(TAG STREQUAL "HEAD" AND IS_DIRECTORY "${REPOSITORY}")
  execute_process(
    COMMAND git -C "${REPOSITORY}" rev-parse HEAD
    OUTPUT_VARIABLE TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  execute_process(
    COMMAND git -C "${REPOSITORY}" status --porcelain --untracked-files=no
    OUTPUT_VARIABLE dirty
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  if(NOT dirty STREQUAL "")
    message(WARNING "Uncommitted changes in ${REPOSITORY} are NOT part of this test (testing commit ${TAG})")
  endif()
elseif(TAG STREQUAL "HEAD")
  execute_process(
    COMMAND git ls-remote "${REPOSITORY}" HEAD
    OUTPUT_VARIABLE remote_head
    OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
  string(REGEX MATCH "^[0-9a-f]+" TAG "${remote_head}")
  if(TAG STREQUAL "")
    message(FATAL_ERROR "cannot resolve HEAD of ${REPOSITORY}")
  endif()
endif()
message(STATUS "FetchContent test: ${REPOSITORY} @ ${TAG}")

file(REMOVE_RECURSE "${BINARY_DIR}")
set(configure_args -S "${SOURCE_DIR}" -B "${BINARY_DIR}" -G "${GENERATOR}"
    "-DVEHICLE_SIL_GIT_REPOSITORY=${REPOSITORY}" "-DVEHICLE_SIL_GIT_TAG=${TAG}")
if(BUILD_TYPE)
  list(APPEND configure_args "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}")
endif()
if(C_COMPILER)
  list(APPEND configure_args "-DCMAKE_C_COMPILER=${C_COMPILER}")
endif()
if(CXX_COMPILER)
  list(APPEND configure_args "-DCMAKE_CXX_COMPILER=${CXX_COMPILER}")
endif()
if(PYTHON)
  list(APPEND configure_args "-DPython3_EXECUTABLE=${PYTHON}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" ${configure_args} COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}" COMMAND_ERROR_IS_FATAL ANY)

# The fetched sources must be exactly the requested commit, with submodules.
execute_process(
  COMMAND git -C "${BINARY_DIR}/_deps/vehicle_sil-src" rev-parse HEAD
  OUTPUT_VARIABLE fetched OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
if(NOT fetched STREQUAL TAG)
  message(FATAL_ERROR "FetchContent checked out ${fetched}, expected ${TAG}")
endif()
execute_process(
  COMMAND git -C "${BINARY_DIR}/_deps/vehicle_sil-src" submodule status --recursive
  OUTPUT_VARIABLE submodules COMMAND_ERROR_IS_FATAL ANY)
message(STATUS "fetched submodules:\n${submodules}")
if(submodules MATCHES "(^|\n)[-+U]")
  message(FATAL_ERROR "FetchContent did not check out every submodule at its pinned commit")
endif()

if(WIN32)
  set(consumer "${BINARY_DIR}/vehicle_sil_consumer.exe")
else()
  set(consumer "${BINARY_DIR}/vehicle_sil_consumer")
endif()
execute_process(COMMAND "${consumer}" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "vehicle_sil_consumer failed with ${result}")
endif()
message(STATUS "FetchContent consumer PASSED")
