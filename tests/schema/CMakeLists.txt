# The JSON Schemas of the simulation configuration and of a scenario (schema/): test_schemas.py holds them against
# the examples, the documents and, with the launcher and the ECUs of examples/ecu_main, against what the launcher's
# readers accept (the keys of every mapping, the samples). It needs a Python 3.11+ with PyYAML and jsonschema: the
# repository's uv environment (.venv) if it has them, else the Python 3 found here (cmake/VehicleSilPython.cmake).
# scripts/verify-python.py runs the part without the launcher too.
vehicle_sil_find_python(_schema_python yaml jsonschema)
if(_schema_python STREQUAL "")
  vehicle_sil_python_install_hint(_hint "pyyaml jsonschema" "the OS packages"
                                  "mingw-w64-ucrt-x86_64-python-yaml mingw-w64-ucrt-x86_64-python-jsonschema"
                                  -DPython3_EXECUTABLE)
  message(STATUS "vehicle-sil: no Python 3.11+ with PyYAML and jsonschema was found (tried: ${_schema_python_TRIED}); "
                 "the tests of the JSON Schemas are skipped (${_hint})")
  return()
endif()

if(TARGET vehicle_sil_launch AND TARGET vehicle_sil_engine_ecu AND TARGET vehicle_sil_dashboard_ecu)
  add_test(NAME schema.files_and_launcher
           COMMAND "${_schema_python}" -B ${CMAKE_CURRENT_SOURCE_DIR}/test_schemas.py)
  set_tests_properties(schema.files_and_launcher PROPERTIES
    LABELS launch TIMEOUT 300
    ENVIRONMENT "VEHICLE_SIL_LAUNCH_EXE=$<TARGET_FILE:vehicle_sil_launch>;VEHICLE_SIL_EXAMPLE_DIR=$<TARGET_FILE_DIR:vehicle_sil_engine_ecu>/..")
else()
  # Without the launcher or the examples: the files only.
  add_test(NAME schema.files
           COMMAND "${_schema_python}" -B ${CMAKE_CURRENT_SOURCE_DIR}/test_schemas.py)
  set_tests_properties(schema.files PROPERTIES LABELS launch TIMEOUT 120)
endif()
