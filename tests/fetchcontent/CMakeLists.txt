# FetchContent tests (opt-in, VEHICLE_SIL_TEST_FETCHCONTENT): configures, builds and runs
# examples/fetchcontent_consumer against a git URL + commit, and the ECU project template against
# this working tree.
set(VEHICLE_SIL_FETCHCONTENT_TEST_REPOSITORY "${PROJECT_SOURCE_DIR}"
    CACHE STRING "Git repository the FetchContent test clones (local path or URL)")
set(VEHICLE_SIL_FETCHCONTENT_TEST_TAG "HEAD"
    CACHE STRING "Commit the FetchContent test checks out; HEAD is resolved at test time")
add_test(
  NAME fetchcontent.consumer
  COMMAND ${CMAKE_COMMAND}
          -DSOURCE_DIR=${PROJECT_SOURCE_DIR}/examples/fetchcontent_consumer
          -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/fetchcontent_consumer
          -DREPOSITORY=${VEHICLE_SIL_FETCHCONTENT_TEST_REPOSITORY}
          -DTAG=${VEHICLE_SIL_FETCHCONTENT_TEST_TAG}
          -DGENERATOR=${CMAKE_GENERATOR}
          -DBUILD_TYPE=${CMAKE_BUILD_TYPE}
          -DC_COMPILER=${CMAKE_C_COMPILER}
          -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
          # The consumer does not use vehicle-sil's .venv; it gets the Python found here.
          -DPYTHON=${VEHICLE_SIL_J1939_CODEGEN_PYTHON}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_consumer.cmake)
set_tests_properties(fetchcontent.consumer PROPERTIES LABELS fetchcontent TIMEOUT 7200)

# The ECU project template as a user has it: copied to a directory of its own, vehicle-sil fetched
# with FetchContent from this working tree (FETCHCONTENT_SOURCE_DIR_VEHICLE_SIL: no network, and
# uncommitted changes count), built, then checked and run with this build's launcher.
if(TARGET vehicle_sil_launch)
  add_test(
    NAME fetchcontent.template
    COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${PROJECT_SOURCE_DIR}
            -DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/template
            -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch>
            -DC_COMPILER=${CMAKE_C_COMPILER}
            -DCXX_COMPILER=${CMAKE_CXX_COMPILER}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/run_template.cmake)
  set_tests_properties(fetchcontent.template PROPERTIES LABELS fetchcontent TIMEOUT 7200)
endif()
