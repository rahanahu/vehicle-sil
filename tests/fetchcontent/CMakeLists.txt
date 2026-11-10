# FetchContent consumer test (opt-in, VEHICLE_SIL_TEST_FETCHCONTENT): configures, builds and runs
# examples/fetchcontent_consumer against a git URL + commit.
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
          -P ${CMAKE_CURRENT_SOURCE_DIR}/run_consumer.cmake)
set_tests_properties(fetchcontent.consumer PROPERTIES LABELS fetchcontent TIMEOUT 7200)
