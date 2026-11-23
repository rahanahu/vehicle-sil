# The scripts of scripts/ that have tests of their own (the others: scripts/verify-python.py runs
# their --help). scripts/run-gui.py: what it says for each thing that is missing, and which
# launcher it takes. scripts/job_summary.py: what the scripts append to GitHub Actions' job summary.
# scripts/check-docs.py: the pairs of an English page and its Japanese page. scripts/verify.py: the
# lines of a failing test's output that its summary shows. scripts/verify-web.py: the Playwright JUnit
# XML summary (--summarize-junit). scripts/hotspots.py: the signals and the
# classification of other work (it makes git repositories and work trees; not run on Windows).
# .github/workflows/ci.yml: the job `all checks` waits for every other job.
# scripts/check-commits.py: the titles and files that CI checks of a pull request (--message, --changed).
# scripts/trace-report.py: the critical path, the hold and the round trips of made-up traces, and its exit codes.
if(VEHICLE_SIL_J1939_CODEGEN_PYTHON)
  add_test(NAME scripts.run_gui
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_run_gui.py)
  set_tests_properties(scripts.run_gui PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.job_summary
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_job_summary.py)
  set_tests_properties(scripts.job_summary PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.check_docs
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_check_docs.py)
  set_tests_properties(scripts.check_docs PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.check_commits
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_check_commits.py)
  set_tests_properties(scripts.check_commits PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.ci_workflow
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_ci_workflow.py)
  set_tests_properties(scripts.ci_workflow PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.verify
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_verify.py)
  set_tests_properties(scripts.verify PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.verify_web
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_verify_web.py)
  set_tests_properties(scripts.verify_web PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME scripts.trace_report
           COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_trace_report.py)
  set_tests_properties(scripts.trace_report PROPERTIES LABELS unit TIMEOUT 120)
  if(NOT WIN32)
    add_test(NAME scripts.hotspots
             COMMAND ${VEHICLE_SIL_J1939_CODEGEN_PYTHON} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_hotspots.py)
    set_tests_properties(scripts.hotspots PROPERTIES LABELS unit TIMEOUT 120)
  endif()
endif()
