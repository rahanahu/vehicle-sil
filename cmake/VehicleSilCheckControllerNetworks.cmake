# Checks that every SIL Kit bus controller that the sources under DIRS make is made on the
# network's name in the simulation (PeerOptions::network_names, --map-network of an ECU
# executable): each line with a call of CreateCanController(, CreateLinController(,
# CreateEthernetController( or CreateFlexrayController( must be in the file FILE and pass the
# network through simulation_name( on that line. A controller made on the name that the ECU's
# code gives would put a renamed network of that kind on the wrong SIL Kit network, silently. The
# check reads the text: a call in a comment counts, which errs on the safe side. A failure names
# the file, the line and what to do.
# Usage: cmake -DDIRS=<directory>[,<directory>...] -DFILE=<file name> -P VehicleSilCheckControllerNetworks.cmake
cmake_minimum_required(VERSION 3.21)
if(NOT DEFINED DIRS OR NOT DEFINED FILE)
  message(FATAL_ERROR "VehicleSilCheckControllerNetworks.cmake: DIRS and FILE are required")
endif()
string(REPLACE "," ";" DIRS "${DIRS}")
set(files "")
foreach(dir IN LISTS DIRS)
  file(GLOB_RECURSE found "${dir}/*.hpp" "${dir}/*.h" "${dir}/*.cpp")
  list(APPEND files ${found})
endforeach()
if(NOT files)
  message(FATAL_ERROR "VehicleSilCheckControllerNetworks.cmake: no sources under ${DIRS}")
endif()
set(bad "")
set(calls 0)
foreach(file IN LISTS files)
  file(STRINGS "${file}" lines REGEX "Create(Can|Lin|Ethernet|Flexray)Controller\\(")
  get_filename_component(name "${file}" NAME)
  foreach(line IN LISTS lines)
    math(EXPR calls "${calls} + 1")
    if(NOT name STREQUAL FILE OR NOT line MATCHES "simulation_name\\(")
      string(STRIP "${line}" shown)
      list(APPEND bad "${name}: ${shown}")
    endif()
  endforeach()
endforeach()
if(bad)
  list(JOIN bad "\n  " text)
  message(FATAL_ERROR "SIL Kit controllers that are not made on the network's name in the simulation:\n  ${text}\n"
                      "Make the controller in ${FILE}, with the network given as simulation_name(<the name the "
                      "ECU's code gives>) on the line of the call (Peer::simulation_name(): the one place that "
                      "applies PeerOptions::network_names), and add the new use of a network to speak_on() and "
                      "hear_on() of tests/simulation/test_network_kinds.cpp, which checks on SIL Kit that a renamed "
                      "network of each use is on the simulation's network.")
endif()
if(calls EQUAL 0)
  message(FATAL_ERROR "VehicleSilCheckControllerNetworks.cmake: no call that makes a SIL Kit controller under "
                      "${DIRS}: the check looks in the wrong place (the calls moved?)")
endif()
