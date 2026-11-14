# The scripts of scripts/ that have tests of their own (the others: scripts/verify-python.py runs
# their --help). scripts/run-gui.py: what it says for each thing that is missing, and which
# launcher it takes. scripts/job_summary.py: what the scripts append to GitHub Actions' job summary.
if(VEHICLE_SIL_J1939_CODEGEN_PYTHON)
  add_test(NAME scripts.run_gui
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_run_gui.py)
  set_tests_properties(scripts.run_gui PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.job_summary
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_job_summary.py)
  set_tests_properties(scripts.job_summary PROPERTIES LABELS unit TIMEOUT 120)
endif()
