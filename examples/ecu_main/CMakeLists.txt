# Two ECUs, each an executable of its own made with vehicle_sil::ecu_main(): the same command
# line (--describe, --registry-uri, --local), so that one person can build each and another can
# put them in one SIL Kit simulation with vehicle_sil_sim_controller.
add_subdirectory(engine)
add_subdirectory(dashboard)
