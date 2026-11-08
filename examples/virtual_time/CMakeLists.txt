# Virtual-time example peers: physics simulator, ECU hosting an existing tick()-based model,
# LIN body controller (master) and LIN sensor (slave). The integration tests run them together.

# The existing ECU model, built as a shared library with a C interface (a DLL on Windows).
add_library(vehicle_sil_example_legacy_ecu SHARED legacy_ecu/legacy_ecu.c)
target_include_directories(vehicle_sil_example_legacy_ecu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/legacy_ecu)
target_compile_definitions(vehicle_sil_example_legacy_ecu PRIVATE VEHICLE_SIL_LEGACY_ECU_BUILD)
set_target_properties(vehicle_sil_example_legacy_ecu PROPERTIES C_VISIBILITY_PRESET hidden C_STANDARD 99)

# Every peer runs on a vehicle_sil::Peer (common/vt_peer.hpp).
set(_vt_common common/vt_common.hpp common/vt_peer.hpp)

add_executable(vehicle_sil_vt_physics physics/main.cpp ${_vt_common})
add_executable(vehicle_sil_vt_ecu ecu/main.cpp ${_vt_common})
target_link_libraries(vehicle_sil_vt_ecu PRIVATE vehicle_sil_example_legacy_ecu)
add_executable(vehicle_sil_vt_lin_sensor lin_sensor/main.cpp ${_vt_common})
add_executable(vehicle_sil_vt_body_controller body_controller/main.cpp ${_vt_common})

foreach(peer IN ITEMS physics ecu lin_sensor body_controller)
  target_link_libraries(vehicle_sil_vt_${peer} PRIVATE vehicle_sil::peer vehicle_sil_cli)
  vehicle_sil_target_defaults(vehicle_sil_vt_${peer})
  vehicle_sil_copy_runtime_dlls(vehicle_sil_vt_${peer})
endforeach()
