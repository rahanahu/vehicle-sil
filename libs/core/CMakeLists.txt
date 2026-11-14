# vehicle_sil::core - SIL Kit participant, registry connection and lifecycle, and the binding of
# a vehicle_sil::step::Host to the participant's steps (bind_host()).
# Protocol independent: must not depend on CAN, LIN or J1939 specifics.
add_library(vehicle_sil_core STATIC
  src/bind_host.cpp
  src/deadline_waiter.cpp
  src/failure_check.cpp
  src/participant_runtime.cpp
  src/simulation_controller.cpp
  src/simulation_watch.cpp
  src/tick_scheduler.cpp
  src/time_gate.cpp
)
add_library(vehicle_sil::core ALIAS vehicle_sil_core)
set_target_properties(vehicle_sil_core PROPERTIES EXPORT_NAME core)
target_include_directories(vehicle_sil_core PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(vehicle_sil_core PUBLIC SilKit::SilKit)
if(WIN32)
  target_link_libraries(vehicle_sil_core PRIVATE winmm) # timeBeginPeriod (deadline_waiter.cpp)
endif()
# The YAML/JSON parser that SIL Kit builds from its ThirdParty directory (rapidyaml, MIT): adds
# vehicle-sil's defaults to a SIL Kit participant configuration of the caller's
# (merged_participant_configuration()). Without the target (SIL Kit not built from source), such a
# configuration is used as it is, without vehicle-sil's checks.
if(TARGET rapidyaml)
  target_link_libraries(vehicle_sil_core PRIVATE rapidyaml)
  target_compile_definitions(vehicle_sil_core PRIVATE VEHICLE_SIL_CORE_HAS_RAPIDYAML)
endif()
vehicle_sil_target_defaults(vehicle_sil_core)

# vehicle_sil::sim_base - simulation time and the step inbox, without SIL Kit. The headers stay
# under vehicle_sil/core/; libraries that must build without SIL Kit (vehicle_sil::can) link
# only this target, so including a SIL Kit dependent header there fails to compile.
find_package(Threads REQUIRED) # StepInbox uses std::mutex
add_library(vehicle_sil_sim_base INTERFACE)
add_library(vehicle_sil::sim_base ALIAS vehicle_sil_sim_base)
set_target_properties(vehicle_sil_sim_base PROPERTIES EXPORT_NAME sim_base)
target_include_directories(vehicle_sil_sim_base INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(vehicle_sil_sim_base INTERFACE cxx_std_20)
target_link_libraries(vehicle_sil_sim_base INTERFACE Threads::Threads)
target_link_libraries(vehicle_sil_core PUBLIC vehicle_sil::sim_base)
# Only src/bind_host.cpp includes vehicle_sil::step's headers (core/bind_host.hpp declares
# step::Host ahead): its callers link vehicle_sil::step (or vehicle_sil::can) themselves.
target_link_libraries(vehicle_sil_core PRIVATE vehicle_sil::step)
