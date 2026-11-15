# The ECU project template (template/): its ECUs build against this tree (as add_subdirectory, the
# way the template's CMakeLists.txt skips FetchContent inside vehicle-sil), and the launcher checks and
# runs its simulation.yaml and its scenario. The check and run output must be the one the guide
# docs/howto/first-ecu.md shows. Copying the template elsewhere and fetching vehicle-sil with
# FetchContent is the opt-in test fetchcontent.template (VEHICLE_SIL_TEST_FETCHCONTENT) and the check
# of scripts/package.py verify.
# The fetch script template/get_tools.py and the template archive of scripts/package.py: template.get_tools.
if(VEHICLE_SIL_J1939_CODEGEN_PYTHON)
  add_test(NAME template.get_tools
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_get_tools.py)
  set_tests_properties(template.get_tools PROPERTIES LABELS unit TIMEOUT 120)
endif()

if(NOT TARGET vehicle_sil_launch)
  return()
endif()
# The project as a user has it: simulation.yaml, the scenarios and the dashboard next to bin/.
set(project_dir ${CMAKE_CURRENT_BINARY_DIR}/project)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${project_dir}/bin)
add_subdirectory(${PROJECT_SOURCE_DIR}/template ${CMAKE_CURRENT_BINARY_DIR}/build)
foreach(target engine_ecu dashboard_ecu)
  # vehicle-sil's warnings (and -Werror with VEHICLE_SIL_WARNINGS_AS_ERRORS) on the template's code too.
  vehicle_sil_target_defaults(${target})
endforeach()
foreach(file simulation.yaml simulation.dashboard.yaml simulation.live.yaml simulation.live.dashboard.yaml
        scenarios/throttle_step.yaml)
  configure_file(${PROJECT_SOURCE_DIR}/template/${file} ${project_dir}/${file} COPYONLY)
endforeach()

add_test(NAME template.check_and_run
         COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch> -DDIR=${project_dir}
                 -DDOC=${VEHICLE_SIL_TESTED_DOCS}/howto/first-ecu.md
                 -DSCENARIO_DOC=${VEHICLE_SIL_TESTED_DOCS}/scenario.md
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/check_and_run.cmake)
set_tests_properties(template.check_and_run PROPERTIES LABELS integration TIMEOUT 300)

# The example of the guide docs/howto/first-ecu.md (examples/first_ecu): the ECU the guide adds to the
# template, built into the same bin/ by the lines the guide appends to the template's CMakeLists.txt.
# The files, cooler_ecu --describe and the launcher's outputs with the three ECUs must be what the
# guide shows. (With a Python that has grpcio, control_server.example_first_ecu runs read_cooler.py.)
add_subdirectory(${PROJECT_SOURCE_DIR}/examples/first_ecu ${CMAKE_CURRENT_BINARY_DIR}/first_ecu_build)
vehicle_sil_target_defaults(cooler_ecu)
add_test(NAME template.first_ecu
         COMMAND ${CMAKE_COMMAND} -DLAUNCH=$<TARGET_FILE:vehicle_sil_launch> -DBIN=${project_dir}/bin
                 -DCOOLER=$<TARGET_FILE:cooler_ecu> -DEXAMPLE=${PROJECT_SOURCE_DIR}/examples/first_ecu
                 -DTEMPLATE=${PROJECT_SOURCE_DIR}/template -DDOC=${VEHICLE_SIL_TESTED_DOCS}/howto/first-ecu.md
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/first_ecu
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/first_ecu.cmake)
set_tests_properties(template.first_ecu PROPERTIES LABELS integration TIMEOUT 300)
