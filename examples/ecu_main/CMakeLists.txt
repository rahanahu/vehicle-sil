# Two ECUs, each an executable of its own made with vehicle_sil::ecu_main(): the same command
# line (--describe, --registry-uri, --local), so that one person can build each and another can
# put them in one SIL Kit simulation with vehicle_sil_sim_controller.
add_subdirectory(engine)
add_subdirectory(dashboard)
# The simulation configuration of the two (vehicle_sil_launch check simulation.yaml), next to
# the executables so that its relative paths find them.
configure_file(simulation.yaml simulation.yaml COPYONLY)
