# Two ECUs, each an executable of its own made with vehicle_sil::ecu_main(): the same command
# line (--describe, --registry-uri, --local), so that one person can build each and another can
# put them in one SIL Kit simulation (vehicle_sil_launch run simulation.yaml).
add_subdirectory(engine)
add_subdirectory(dashboard)
# The simulation configuration of the two (vehicle_sil_launch check / run simulation.yaml), next
# to the executables so that its relative paths find them.
configure_file(simulation.yaml simulation.yaml COPYONLY)
# A scenario for the two (vehicle_sil_launch run simulation.yaml --scenario scenarios/throttle_step.yaml).
configure_file(scenarios/throttle_step.yaml scenarios/throttle_step.yaml COPYONLY)
# The browser GUI's dashboard of simulation.yaml and the panel it loads (docs/gui.md).
configure_file(simulation.dashboard.yaml simulation.dashboard.yaml COPYONLY)
configure_file(panels/rpm-bar.js panels/rpm-bar.js COPYONLY)
