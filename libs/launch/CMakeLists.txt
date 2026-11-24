# vehicle_sil_launch (library) - the use cases of vehicle_sil_launch (check, run, test, probe, connections),
# the request and the output for people and as JSON. The run itself is vehicle_sil::runner
# (libs/runner); the configuration file and its check are vehicle_sil::config (libs/config); the
# scenario is vehicle_sil::scenario (libs/scenario). Internal; not part of the library API. The file format: docs/simulation-config.md; the rules: docs/simulation-check.md.
if(NOT TARGET rapidyaml)
  message(STATUS "vehicle-sil: SIL Kit's rapidyaml target is not available (SIL Kit not built from source); vehicle_sil_launch is skipped")
  return()
endif()

# The five parts of src/ (the include direction between them is kept by the tests in tests/launch):
# usecases/ (check, run, serve, test, connections: calls the others in order), request/ (the request and its
# reading), probe/ (looks at the computer), format/ (the outputs, the exit codes), endpoints/ (stdin
# commands and the Control Server).
add_library(vehicle_sil_launch_lib STATIC
  src/usecases/check.cpp
  src/usecases/connections.cpp
  src/usecases/execution_runtime.cpp
  src/usecases/run_file.cpp
  src/usecases/serve.cpp
  src/usecases/sha256.cpp
  src/usecases/test.cpp
  src/request/request.cpp
  src/request/request_read.cpp
  src/probe/probe.cpp
  src/probe/saved_descriptions.cpp
  src/format/check_format.cpp
  src/format/error_result.cpp
  src/format/events.cpp
  src/format/reported_networks.cpp
  src/format/run_format.cpp
  src/format/run_report.cpp
  src/format/timeline_format.cpp
  src/endpoints/command_input.cpp
)
add_library(vehicle_sil::launch ALIAS vehicle_sil_launch_lib)
target_include_directories(vehicle_sil_launch_lib PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
# The private headers between the parts are included as "format/check_format.hpp" and the like.
target_include_directories(vehicle_sil_launch_lib PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# The run (vehicle_sil/runner/...: the plan, the controls, the events, the outcome), the configuration
# and its check (vehicle_sil/config/...) and the scenario (vehicle_sil/scenario/...), which the public
# headers include. The runner brings control and the simulation time with it.
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::runner vehicle_sil::scenario vehicle_sil::config)
# The process headers: events.hpp, a public header, includes process.hpp (child_process.hpp and paths.hpp
# are used by the probe, the test and the output).
target_link_libraries(vehicle_sil_launch_lib PUBLIC vehicle_sil::process)
# The description of an ECU (describe.hpp, describe_json.hpp) that the outputs repeat.
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::ecu)
# The JSON writer that the launcher shares with --describe (vehicle_sil/json/json_writer.hpp).
target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::json)
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT); the
# public headers do not include it.
target_link_libraries(vehicle_sil_launch_lib PRIVATE rapidyaml)
# The Control Server of run --control (src/endpoints/control_server.cpp): gRPC on the run's hub. Only with
# VEHICLE_SIL_BUILD_CONTROL_SERVER; no public header includes control_grpc's. Without it, a run
# that is asked for a control server has none to make (control_server_none.cpp).
if(TARGET vehicle_sil::control_grpc)
  target_sources(vehicle_sil_launch_lib PRIVATE src/endpoints/control_server.cpp)
  target_link_libraries(vehicle_sil_launch_lib PRIVATE vehicle_sil::control_grpc)
else()
  target_sources(vehicle_sil_launch_lib PRIVATE src/endpoints/control_server_none.cpp)
endif()
vehicle_sil_target_defaults(vehicle_sil_launch_lib)
