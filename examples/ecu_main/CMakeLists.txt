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
# Taking the engine off the bus and back (--scenario scenarios/engine_disconnect.yaml).
configure_file(scenarios/engine_disconnect.yaml scenarios/engine_disconnect.yaml COPYONLY)
# The browser GUI's dashboard of simulation.yaml and the panel it loads (docs/gui.md).
configure_file(simulation.dashboard.yaml simulation.dashboard.yaml COPYONLY)
# The same two at the speed of a clock and without an end, to watch in the GUI (scripts/run-gui.py).
# The GUI reads <configuration>.dashboard.yaml: the same dashboard under that name.
configure_file(simulation.live.yaml simulation.live.yaml COPYONLY)
configure_file(simulation.dashboard.yaml simulation.live.dashboard.yaml COPYONLY)
configure_file(panels/rpm-bar.js panels/rpm-bar.js COPYONLY)
# Files with mistakes in them that the tests launch_cli.docs and launch_run.scenario once wrote here
# (they now write to their own work directories): taken out of a build directory from before that, so
# that nothing that fails on purpose lies next to the example. Only these four names; anything else
# here is left alone. This can go once no build directory from before this line is in use.
file(REMOVE ${CMAKE_CURRENT_BINARY_DIR}/two-engines.yaml ${CMAKE_CURRENT_BINARY_DIR}/engine-alone.yaml
            ${CMAKE_CURRENT_BINARY_DIR}/throttle_mistakes.yaml ${CMAKE_CURRENT_BINARY_DIR}/throttle_response.yaml)
