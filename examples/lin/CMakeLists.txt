# LIN example peers: a master and slaves of one LIN cluster in virtual time (schedule, master and
# slave frames, sleep and wakeup). They use vehicle_sil::lin on SIL Kit (vehicle_sil::lin_silkit)
# only, not J1939 and not Peer. The integration tests run them together.
set(_lin_common common/lin_cluster.hpp ../virtual_time/common/vt_common.hpp)

add_executable(vehicle_sil_lin_master master/main.cpp ${_lin_common})
add_executable(vehicle_sil_lin_slave slave/main.cpp ${_lin_common})

foreach(peer IN ITEMS master slave)
  target_link_libraries(vehicle_sil_lin_${peer} PRIVATE vehicle_sil::lin_silkit vehicle_sil_cli)
  vehicle_sil_target_defaults(vehicle_sil_lin_${peer})
  vehicle_sil_copy_runtime_dlls(vehicle_sil_lin_${peer})
endforeach()
