# vehicle_sil::signal - the usual test signals as functions of time (sine, triangle, sawtooth,
# square, noise, chirp, first-order lag, contact bounce, a table of points). Header only, standard
# library only: it links nothing, so a participant and the launcher's scenarios can both use it.
add_library(vehicle_sil_signal INTERFACE)
add_library(vehicle_sil::signal ALIAS vehicle_sil_signal)
set_target_properties(vehicle_sil_signal PROPERTIES EXPORT_NAME signal)
target_include_directories(vehicle_sil_signal INTERFACE $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(vehicle_sil_signal INTERFACE cxx_std_20)
